#include "AnkiHomeActivity.h"

#if CROSSINK_APP_CAP_ANKI
#include <I18n.h>
#include <Memory.h>

#include <algorithm>
#include <cstdio>

#include "AnkiReviewActivity.h"
#include "AnkiUi.h"
#include "activities/util/ConfirmationActivity.h"
#include "components/CompactHeader.h"
#include "components/TouchHeaderBackButton.h"
#include "components/UITheme.h"
#include "components/UIThemeTokens.h"
#include "components/UiAppHelpers.h"
#include "util/InputReleaseGuard.h"

namespace fui = freeink::ui;
namespace {
constexpr fui::ActionId kRow = 1;
}

AnkiHomeActivity::AnkiHomeActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("AnkiHome", renderer, mappedInput),
      uiTarget(makeUiTarget(renderer)),
      app(uiTarget, uiTarget.deviceContext()) {}

int AnkiHomeActivity::deckRowOffset() const { return store.needsAttention() ? 3 : 2; }

int AnkiHomeActivity::rowCount() const { return deckRowOffset() + store.deckCount(); }

void AnkiHomeActivity::reload() {
  storageError = !store.open();
  selectedIndex = std::clamp(selectedIndex, 0, std::max(0, rowCount() - 1));
  topIndex = scrollListBy(topIndex, 0, visibleRows, rowCount());
  uiReady = false;
}

void AnkiHomeActivity::onEnter() {
  Activity::onEnter();
  ignoreConfirmRelease = mappedInput.isPressed(MappedInputManager::Button::Confirm);
  reload();
  applySharedUiTheme(app, uiTarget);
  app.on(kRow, onRow, this);
  app.setScreen(screen, this);
  requestUpdate();
}

void AnkiHomeActivity::onExit() {
  store.close();
  Activity::onExit();
}

void AnkiHomeActivity::onRow(const fui::ActionEvent& event, void* context) {
  auto* self = static_cast<AnkiHomeActivity*>(context);
  if (event.value < 0 || event.value >= self->rowCount()) return;
  self->selectedIndex = event.value;
  self->app.clearTapFlash();
  self->activateRow();
}

void AnkiHomeActivity::activateRow() {
  actionError = false;
  if (selectedIndex == 0 || selectedIndex == 1) {
    activityManager.goToAnkiSync(selectedIndex == 1);
    return;
  }
  if (store.needsAttention() && selectedIndex == 2) {
    openRecovery();
    return;
  }
  if (storageError || store.needsAttention()) return;
  if (!store.readDeck(static_cast<uint16_t>(selectedIndex - deckRowOffset()), rowDeck)) {
    LOG_ERR("ANKI", "Cannot read selected deck");
    storageError = true;
    requestUpdate();
    return;
  }
  auto review = makeUniqueNoThrow<AnkiReviewActivity>(renderer, mappedInput, rowDeck.deckId);
  if (!review) {
    LOG_ERR("ANKI", "Cannot allocate review activity");
    actionError = true;
    requestUpdate();
    return;
  }
  store.close();
  startActivityForResult(std::move(review), [this](const ActivityResult&) {
    RenderLock lock(*this);
    reload();
  });
}

void AnkiHomeActivity::openRecovery() {
  if (!store.exportRecovery()) {
    LOG_ERR("ANKI", "Cannot export review recovery report");
    actionError = true;
    requestUpdate();
    return;
  }
  auto confirmation = makeUniqueNoThrow<ConfirmationActivity>(renderer, mappedInput, tr(STR_ANKI_HANDLED_IN_DESKTOP),
                                                              tr(STR_ANKI_RECOVERY_CONFIRM));
  if (!confirmation) {
    LOG_ERR("ANKI", "Cannot allocate recovery confirmation");
    actionError = true;
    requestUpdate();
    return;
  }
  store.close();
  startActivityForResult(std::move(confirmation), [this](const ActivityResult& result) {
    RenderLock lock(*this);
    reload();
    if (!result.isCancelled && !storageError) {
      actionError = !store.resolveAttention();
      if (actionError) LOG_ERR("ANKI", "Cannot mark exported reviews handled");
      reload();
    }
  });
}

void AnkiHomeActivity::loop() {
  if (TouchHeaderBackButton::wasTapped(mappedInput, TouchHeaderBackButton::compactHeaderRect(renderer)) ||
      mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    onGoHome();
    return;
  }
  RenderLock lock(*this);
  if (uiReady) {
    const auto snapshot = touchSnapshotFrom(mappedInput);
    if (snapshot.touchPressed || snapshot.touchReleased) {
      const auto event = app.route(snapshot);
      if (app.invalidated()) requestUpdate();
      if (event) return;
    }
  }
  if (InputReleaseGuard::consumeInitialRelease(mappedInput, MappedInputManager::Button::Confirm, ignoreConfirmRelease))
    return;
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    activateRow();
    return;
  }
  const auto swipe = mappedInput.wasSwipe();
  if (swipe == MappedInputManager::SwipeDir::Up || swipe == MappedInputManager::SwipeDir::Down) {
    topIndex = scrollListBy(topIndex, swipe == MappedInputManager::SwipeDir::Up ? visibleRows : -visibleRows,
                            visibleRows, rowCount());
    requestUpdate();
    return;
  }
  navigator.onNextRelease([this] {
    selectedIndex = ButtonNavigator::nextIndex(selectedIndex, rowCount());
    topIndex = followListSelection(selectedIndex, topIndex, visibleRows, rowCount());
    requestUpdate();
  });
  navigator.onPreviousRelease([this] {
    selectedIndex = ButtonNavigator::previousIndex(selectedIndex, rowCount());
    topIndex = followListSelection(selectedIndex, topIndex, visibleRows, rowCount());
    requestUpdate();
  });
}

void AnkiHomeActivity::row(void* context, const uint16_t index, fui::ListItem& item) {
  auto* self = static_cast<AnkiHomeActivity*>(context);
  item.actionValue = static_cast<int16_t>(index);
  if (index == 0) {
    item.label = tr(STR_ANKI_SYNC);
    snprintf(self->rowValue, sizeof(self->rowValue), "%lu", static_cast<unsigned long>(self->store.pendingCount()));
    item.value = self->rowValue;
    item.subtitle = tr(STR_ANKI_PENDING_REVIEWS);
  } else if (index == 1) {
    item.label = tr(STR_ANKI_CONNECTION);
  } else if (self->store.needsAttention() && index == 2) {
    item.label = tr(STR_ANKI_RECOVERY);
  } else if (self->store.readDeck(index - self->deckRowOffset(), self->rowDeck)) {
    item.label = self->rowDeck.name;
    snprintf(self->rowValue, sizeof(self->rowValue), "%u/%u", self->rowDeck.remaining, self->rowDeck.cardCount);
    item.value = self->rowValue;
    if (self->rowDeck.remaining == 0) item.subtitle = tr(STR_ANKI_REPEAT_DECK);
    item.enabled = self->rowDeck.cardCount > 0 && !self->store.needsAttention();
  } else {
    item.label = tr(STR_ANKI_STORAGE_ERROR);
    item.enabled = false;
  }
}

void AnkiHomeActivity::screen(UiApp::ScreenType& screen, void* context) {
  auto* self = static_cast<AnkiHomeActivity*>(context);
  const auto& metrics = UITheme::getInstance().getMetrics();
  screen.setContentMargin(fui::Insets{static_cast<int16_t>(CompactHeader::contentTop(metrics)), 0,
                                      static_cast<int16_t>(metrics.buttonHintsHeight), 0});
  if (!self->store.hasSnapshot() || self->storageError) {
    const char* message = self->storageError ? tr(STR_ANKI_STORAGE_ERROR) : tr(STR_ANKI_NO_CARDS);
    const auto bounds = screen.body().inset(fui::Insets{0, static_cast<int16_t>(metrics.contentSidePadding), 0,
                                                        static_cast<int16_t>(metrics.contentSidePadding)});
    const auto style = anki::ui::messageStyle(screen.target(), bounds, screen.theme().bodyText);
    const auto size = fui::measureWrappedText(screen.target(), message, style, bounds.width);
    auto band = screen.takeTop(std::max(screen.theme().rowHeight, size.height), screen.theme().spaceSm);
    band.x = bounds.x;
    band.width = bounds.width;
    anki::ui::drawMessage(screen.target(), band, message, style);
  }
  fui::ListProps props;
  props.count = static_cast<uint16_t>(self->rowCount());
  props.rowProvider = row;
  props.rowProviderCtx = self;
  props.selectedIndex = static_cast<int16_t>(self->selectedIndex);
  props.action = kRow;
  props.inputMask = fui::InputTouch;
  props.labelText = screen.theme().bodyText;
  props.labelText.maxLines = 2;
  props.subtitleText = screen.theme().smallText;
  props.subtitleText.maxLines = 2;
  self->visibleRows = std::max(1, static_cast<int>(configureUiList(props, screen.theme(), screen.body())));
  self->topIndex = scrollListBy(self->topIndex, 0, self->visibleRows, self->rowCount());
  props.topIndex = static_cast<uint16_t>(self->topIndex);
  screen.list(props);
  if (self->actionError) {
    const char* message = tr(STR_ANKI_SAVE_FAILED);
    fui::PopupProps popup;
    popup.styles = screen.theme().popup;
    const auto bounds = screen.body();
    const int16_t width = bounds.width * 3 / 4;
    const int16_t textWidth = std::max(1, width - popup.padding.left - popup.padding.right);
    auto style = anki::ui::messageStyle(screen.target(), bounds, screen.theme().bodyText);
    style.align = fui::TextAlign::Center;
    const auto size = fui::measureWrappedText(screen.target(), message, style, textWidth);
    const auto panel = fui::centeredRect(
        bounds, fui::Size{static_cast<int16_t>(size.width + popup.padding.left + popup.padding.right),
                          static_cast<int16_t>(size.height + popup.padding.top + popup.padding.bottom)});
    // Paint SDK chrome without its allocating multiline text adapter: this
    // error can itself follow an allocation failure when opening a review.
    fui::popup(screen.frame(), panel, popup);
    anki::ui::drawMessage(screen.target(), panel.inset(popup.padding), message, style);
  }
}

void AnkiHomeActivity::render(RenderLock&&) {
  renderer.clearScreen();
  TouchHeaderBackButton::drawCompact(renderer, tr(STR_ANKI));
  app.setDevice(uiTarget.deviceContext());
  uiReady = false;
  app.render();
  uiReady = true;
  const auto labels =
      mappedInput.mapLabels(mappedInput.withBackArrow(tr(STR_HOME)), tr(STR_OPEN), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
#endif
