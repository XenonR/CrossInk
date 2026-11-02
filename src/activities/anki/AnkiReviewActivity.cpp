#include "AnkiReviewActivity.h"

#if CROSSINK_APP_CAP_ANKI
#include <I18n.h>
#include <Memory.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>

#include "AnkiUi.h"
#include "components/CompactHeader.h"
#include "components/TouchHeaderBackButton.h"
#include "components/UIScale.h"
#include "components/UITheme.h"
#include "components/UiAppHelpers.h"
#include "fontIds.h"
#include "util/InputReleaseGuard.h"

namespace fui = freeink::ui;
namespace {
constexpr fui::ActionId kAction = 1;
constexpr int kReveal = 2;
constexpr int kAgain = 3;
constexpr int kGood = 5;
constexpr int kLastAction = 6;
constexpr int kRepeat = 7;
constexpr unsigned long kMaxReviewMs = 3600000;
constexpr time_t kEarliestValidEpoch = 1704067200;  // 2024-01-01 UTC
}  // namespace

AnkiReviewActivity::AnkiReviewActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const int64_t deckId)
    : Activity("AnkiReview", renderer, mappedInput),
      deckId(deckId),
      uiTarget(makeUiTarget(renderer)),
      app(uiTarget, uiTarget.deviceContext()) {}

void AnkiReviewActivity::onEnter() {
  Activity::onEnter();
  ignoreConfirmRelease = mappedInput.isPressed(MappedInputManager::Button::Confirm);
  side = makeUniqueNoThrow<char[]>(anki::MAX_SIDE_BYTES + 1);
  if (!side) {
    LOG_ERR("ANKI", "Cannot allocate %u-byte card-side buffer", static_cast<unsigned>(anki::MAX_SIDE_BYTES + 1));
    storageError = true;
  } else if (!store.open() || store.needsAttention() || !store.beginReview(deckId)) {
    LOG_ERR("ANKI", "Cannot start review from the SD snapshot");
    storageError = true;
  } else {
    loadNextCard();
  }
  applySharedUiTheme(app, uiTarget);
  app.on(kAction, onAction, this);
  app.setScreen(screen, this);
  requestUpdate();
}

void AnkiReviewActivity::onExit() {
  store.close();
  side.reset();
  Activity::onExit();
}

void AnkiReviewActivity::loadNextCard() {
  uiReady = false;
  done = false;
  answer = false;
  selectingRating = false;
  focusedAction = kReveal;
  // A Confirm held during a touch rating owns its release on the next screen.
  ignoreConfirmRelease = mappedInput.isPressed(MappedInputManager::Button::Confirm);
  if (!store.nextReviewCard(card)) {
    storageError = store.failed();
    done = !storageError;
    if (storageError) LOG_ERR("ANKI", "Cannot locate next review card");
    return;
  }
  cardStartedMs = millis();
  loadSide();
}

void AnkiReviewActivity::loadSide() {
  uiReady = false;
  pageLayoutValid = false;
  if (!store.readSide(card.ordinal, answer, side.get(), anki::MAX_SIDE_BYTES + 1)) {
    LOG_ERR("ANKI", "Cannot read card %u %s", card.ordinal, answer ? "answer" : "question");
    storageError = true;
    return;
  }
  anki::text::normalizePreparedText(side.get());
  sideLength = strlen(side.get());
  pageStart = {};
  page = {};
  pageNumber = 0;
}

bool AnkiReviewActivity::actionEnabled(const int action) const {
  if (done) return action == kRepeat;
  return answer ? action >= kAgain && action <= kLastAction : action == kReveal;
}

void AnkiReviewActivity::moveFocus(const int delta) {
  focusedAction = kAgain + (focusedAction - kAgain + delta + 4) % 4;
  uiReady = false;
  requestUpdate();
}

int AnkiReviewActivity::measure(void* context, const char* text, const size_t length, const bool bold) {
  auto* self = static_cast<AnkiReviewActivity*>(context);
  // All spans borrow the activity's mutable buffer. The render lock protects
  // temporary termination while GfxRenderer consumes its C-string API.
  auto* end = const_cast<char*>(text + length);
  const char saved = *end;
  *end = '\0';
  const int result =
      self->renderer.getTextAdvanceX(self->textFontId, text, bold ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
  *end = saved;
  return result;
}

void AnkiReviewActivity::drawRun(void* context, const char* text, const size_t length, const bool bold) {
  auto* self = static_cast<AnkiReviewActivity*>(context);
  auto* end = const_cast<char*>(text + length);
  const char saved = *end;
  *end = '\0';
  const auto style = bold ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
  self->renderer.drawText(self->textFontId, self->drawX, self->drawY, text, true, style);
  self->drawX += self->renderer.getTextAdvanceX(self->textFontId, text, style);
  *end = saved;
}

void AnkiReviewActivity::changePage(const bool next) {
  if (next) {
    if (!page.hasMore) return;
    pageStart = page.next;
    ++pageNumber;
  } else {
    if (pageNumber == 0) return;
    anki::text::Cursor cursor{};
    anki::text::Cursor previous{};
    while (cursor.offset < pageStart.offset) {
      previous = cursor;
      const auto candidate =
          anki::text::layoutPage(side.get(), sideLength, cursor, textWidth, lines.data(), rowsPerPage, {this, measure});
      if (candidate.next.offset <= cursor.offset) break;
      cursor = candidate.next;
    }
    pageStart = previous;
    --pageNumber;
  }
  uiReady = false;
  pageLayoutValid = false;
  requestUpdate();
}

void AnkiReviewActivity::activateAction(const int action) {
  if (storageError || saveError) {
    finish();
    return;
  }
  if (!actionEnabled(action)) return;
  if (action == kRepeat) {
    if (!store.restartDeck(deckId)) {
      LOG_ERR("ANKI", "Cannot persist deck restart");
      saveError = true;
    } else {
      loadNextCard();
    }
    requestUpdate();
  } else if (action == kReveal) {
    answer = true;
    selectingRating = false;
    focusedAction = kGood;
    loadSide();
    requestUpdate();
  } else {
    const time_t now = time(nullptr);
    const uint64_t timestamp = now >= kEarliestValidEpoch ? static_cast<uint64_t>(now) * 1000 : 0;
    const unsigned long elapsed = std::min(millis() - cardStartedMs, kMaxReviewMs);
    if (!store.recordReview(card.ordinal, static_cast<uint8_t>(action - kAgain + 1), elapsed, timestamp)) {
      LOG_ERR("ANKI", "Cannot persist rating for card %u", card.ordinal);
      saveError = true;
    } else {
      loadNextCard();
    }
    requestUpdate();
  }
}

void AnkiReviewActivity::onAction(const fui::ActionEvent& event, void* context) {
  auto* self = static_cast<AnkiReviewActivity*>(context);
  self->app.clearTapFlash();
  self->focusedAction = event.value;
  self->activateAction(event.value);
}

void AnkiReviewActivity::loop() {
  if (TouchHeaderBackButton::wasTapped(mappedInput, TouchHeaderBackButton::compactHeaderRect(renderer)) ||
      mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
    return;
  }
  RenderLock lock(*this);
  if (InputReleaseGuard::consumeInitialRelease(mappedInput, MappedInputManager::Button::Confirm, ignoreConfirmRelease))
    return;
  // A button burst must not reveal, select or grade the next unseen screen.
  // Back and error dismissal remain available while a redraw is pending.
  if (!uiReady && !storageError && !saveError) return;
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (!done && !storageError && !saveError && answer && !selectingRating) {
      selectingRating = true;
      focusedAction = kGood;
      uiReady = false;
      requestUpdate();
    } else {
      activateAction(done ? kRepeat : (answer ? focusedAction : kReveal));
    }
    return;
  }
  if (storageError || saveError) return;
  if (uiReady) {
    const auto snapshot = touchSnapshotFrom(mappedInput);
    if (snapshot.touchPressed || snapshot.touchReleased) {
      const auto event = app.route(snapshot);
      if (app.invalidated()) requestUpdate();
      if (event) return;
    }
  }
  if (done) return;
  const auto swipe = mappedInput.wasSwipe();
  if (swipe == MappedInputManager::SwipeDir::Up || swipe == MappedInputManager::SwipeDir::Left) {
    changePage(true);
    return;
  }
  if (swipe == MappedInputManager::SwipeDir::Down || swipe == MappedInputManager::SwipeDir::Right) {
    changePage(false);
    return;
  }
  // Side-button releases also match PageForward/PageBack. Resolve the rating
  // mode first so those aliases cannot turn a rating change into page navigation.
  if (selectingRating) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Down) ||
        mappedInput.wasReleased(MappedInputManager::Button::Right))
      moveFocus(1);
    else if (mappedInput.wasReleased(MappedInputManager::Button::Up) ||
             mappedInput.wasReleased(MappedInputManager::Button::Left))
      moveFocus(-1);
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::PageForward) ||
      mappedInput.wasReleased(MappedInputManager::Button::Right) ||
      mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    changePage(true);
  } else if (mappedInput.wasReleased(MappedInputManager::Button::PageBack) ||
             mappedInput.wasReleased(MappedInputManager::Button::Left) ||
             mappedInput.wasReleased(MappedInputManager::Button::Up)) {
    changePage(false);
  }
}

void AnkiReviewActivity::drawAction(UiApp::ScreenType& screen, const fui::Rect rect, const int action,
                                    const char* label) {
#ifdef SIMULATOR
  actionRects[action] = rect;
#endif
  fui::ButtonProps props;
  props.label = label;
  props.action = kAction;
  props.value = static_cast<int16_t>(action);
  props.enabled = actionEnabled(action);
  props.inputMask = fui::InputTouch;
  props.state = selectingRating && focusedAction == action ? fui::StateSelected : fui::StateNormal;
  props.text = screen.theme().smallText;
  screen.button(props, rect);
}

void AnkiReviewActivity::drawCard(UiApp::ScreenType& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  int top, right, bottom, left;
  renderer.getOrientedViewableTRBL(&top, &right, &bottom, &left);
  const int x = left + metrics.contentSidePadding;
  const int width = std::max(1, renderer.getScreenWidth() - x - right - metrics.contentSidePadding);
  const int headerBottom = std::max(top, CompactHeader::contentTop(metrics));
  const int footerBottom = renderer.getScreenHeight() - bottom - metrics.buttonHintsHeight - metrics.verticalSpacing;
  const int buttonHeight = std::max(44, static_cast<int>(screen.theme().rowHeight));
  const int gap = std::max(4, static_cast<int>(screen.theme().spaceSm));
  const int ratingsY = footerBottom - buttonHeight;
  const int lineHeight = std::max(1, renderer.getLineHeight(uiScaleSpec().bodyFontId));
  const int availableHeight = std::max(lineHeight, ratingsY - headerBottom - gap);
  if (textWidth != width || textHeight != availableHeight || textFontId != uiScaleSpec().bodyFontId) {
    textWidth = width;
    textHeight = availableHeight;
    textFontId = uiScaleSpec().bodyFontId;
    rowsPerPage = std::min(kLineCapacity, static_cast<size_t>(availableHeight / lineHeight));
    pageStart = {};
    pageNumber = 0;
    pageLayoutValid = false;
  }
  if (!pageLayoutValid) {
    // Focus/tap redraws reuse the borrowed line offsets. Text measurement can
    // perform contextual shaping, so repeat it only when the page changes.
    page = anki::text::layoutPage(side.get(), sideLength, pageStart, textWidth, lines.data(), rowsPerPage,
                                  {this, measure});
    pageLayoutValid = true;
#ifdef SIMULATOR
    ++layoutPasses;
#endif
  }
  if (!actionEnabled(focusedAction)) focusedAction = answer ? kGood : kReveal;
  renderer.beginTextClip(x, headerBottom, width, availableHeight);
  for (size_t i = 0; i < page.lineCount; ++i) {
    drawX = x;
    drawY = headerBottom + static_cast<int>(i) * lineHeight;
    anki::text::visitRuns(side.get(), lines[i], {this, drawRun});
  }
  renderer.endTextClip();

  if (!answer) {
    drawAction(screen,
               fui::Rect{static_cast<int16_t>(x), static_cast<int16_t>(ratingsY), static_cast<int16_t>(width),
                         static_cast<int16_t>(buttonHeight)},
               kReveal, tr(STR_ANKI_SHOW_ANSWER));
  } else {
    const char* labels[] = {tr(STR_ANKI_AGAIN), tr(STR_ANKI_HARD), tr(STR_ANKI_GOOD), tr(STR_ANKI_EASY)};
    const int ratingWidth = (width - gap * 3) / 4;
    for (int i = 0; i < 4; ++i) {
      drawAction(screen,
                 fui::Rect{static_cast<int16_t>(x + i * (ratingWidth + gap)), static_cast<int16_t>(ratingsY),
                           static_cast<int16_t>(ratingWidth), static_cast<int16_t>(buttonHeight)},
                 kAgain + i, labels[i]);
    }
  }
}

void AnkiReviewActivity::screen(UiApp::ScreenType& screen, void* context) {
  auto* self = static_cast<AnkiReviewActivity*>(context);
  if (self->storageError || self->saveError || self->done) {
    const auto& metrics = UITheme::getInstance().getMetrics();
    screen.setContentMarginFromScreen(
        fui::Insets{static_cast<int16_t>(CompactHeader::contentTop(metrics) + metrics.verticalSpacing),
                    static_cast<int16_t>(metrics.contentSidePadding),
                    static_cast<int16_t>(metrics.buttonHintsHeight + metrics.verticalSpacing),
                    static_cast<int16_t>(metrics.contentSidePadding)});
    const char* message = self->saveError
                              ? tr(STR_ANKI_SAVE_FAILED)
                              : (self->storageError ? tr(STR_ANKI_STORAGE_ERROR) : tr(STR_ANKI_SESSION_DONE));
    auto style = screen.theme().bodyText;
    style.align = fui::TextAlign::Center;
    auto bounds = screen.body();
    if (self->done && !self->storageError && !self->saveError) {
      const int16_t height = std::max(44, static_cast<int>(screen.theme().rowHeight));
      const fui::Rect button{bounds.x, static_cast<int16_t>(bounds.y + bounds.height - height), bounds.width, height};
      self->focusedAction = kRepeat;
      self->drawAction(screen, button, kRepeat, tr(STR_ANKI_REPEAT_DECK));
      bounds.height = std::max(0, bounds.height - height - screen.theme().spaceSm);
    }
    anki::ui::drawMessage(screen.target(), bounds, message, style);
    return;
  }
  self->drawCard(screen);
}

void AnkiReviewActivity::render(RenderLock&&) {
  renderer.clearScreen();
  char title[96];
  snprintf(title, sizeof(title), "%s — %s", tr(STR_ANKI), answer ? tr(STR_ANKI_ANSWER) : tr(STR_ANKI_QUESTION));
  TouchHeaderBackButton::drawCompact(renderer, done ? tr(STR_ANKI) : title);
  app.setDevice(uiTarget.deviceContext());
  uiReady = false;
  app.render();
  uiReady = true;
  const char* confirmLabel = storageError || saveError ? tr(STR_BACK)
                             : done                    ? tr(STR_ANKI_REPEAT_DECK)
                             : answer                  ? tr(STR_SELECT)
                                                       : tr(STR_ANKI_SHOW_ANSWER);
  const auto labels = mappedInput.mapLabels(mappedInput.withBackArrow(tr(STR_BACK)), confirmLabel,
                                            selectingRating ? tr(STR_DIR_UP) : tr(STR_PREV_PAGE),
                                            selectingRating ? tr(STR_DIR_DOWN) : tr(STR_NEXT_PAGE));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer(refresh.modeFor(static_cast<uint8_t>((card.ordinal * 2 + (answer ? 1 : 0)) % 251)));
}
#endif
