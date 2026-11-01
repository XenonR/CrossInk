#include "AnkiSyncActivity.h"

#if CROSSINK_APP_CAP_ANKI
#include <I18n.h>
#include <Memory.h>
#include <WiFi.h>
#include <esp_task_wdt.h>

#include "AnkiUi.h"
#include "SdCardFontSystem.h"
#include "SilentRestart.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/util/ConfirmationActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/CompactHeader.h"
#include "components/TouchHeaderBackButton.h"
#include "components/UITheme.h"
#include "components/UiAppHelpers.h"
#include "util/InputReleaseGuard.h"

namespace fui = freeink::ui;
namespace {
constexpr fui::ActionId ROW_ACTION = 1;
constexpr uint32_t WORKER_STACK_BYTES = 4096;
}  // namespace

AnkiSyncActivity::AnkiSyncActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const bool configureOnly)
    : Activity("AnkiSync", renderer, mappedInput),
      uiTarget(makeUiTarget(renderer)),
      app(uiTarget, uiTarget.deviceContext()),
      configureOnly(configureOnly) {}

void AnkiSyncActivity::onEnter() {
  Activity::onEnter();
  sdFontSystem.releaseLoadedFont(renderer);
  ignoreConfirmRelease = mappedInput.isPressed(MappedInputManager::Button::Confirm);
  applySharedUiTheme(app, uiTarget);
  app.on(ROW_ACTION, onRow, this);
  app.setScreen(screen, this);
  if (!store.open() || !anki::AnkiConnectionStore::load(connection)) {
    outcome = anki::SyncResult::StorageError;
    hasOutcome = true;
  } else if (store.needsAttention()) {
    outcome = anki::SyncResult::Attention;
    hasOutcome = true;
  } else if (!configureOnly && connection.configured()) {
    launchSync();
  }
  requestUpdate();
}

void AnkiSyncActivity::joinWorker() {
  if (!worker) return;
  cancel = true;
  while (!done.load(std::memory_order_acquire)) {
    esp_task_wdt_reset();
    delay(10);
  }
#ifdef SIMULATOR
  // The host task shim needs its thread handle released by its owner.
  vTaskDelete(worker);
#endif
  worker = nullptr;
  running = false;
}

void AnkiSyncActivity::onExit() {
  joinWorker();
  client.reset();
  store.close();
  Activity::onExit();
  if (networkTouched && WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
#ifdef SIMULATOR
    WiFi.mode(WIFI_OFF);
#else
    silentRestartToAnki();
#endif
  }
}

void AnkiSyncActivity::saveConnection(anki::Connection candidate) {
  if (!anki::AnkiConnectionStore::normalizeUrl(candidate.url) ||
      !anki::AnkiConnectionStore::validToken(candidate.token)) {
    invalidConfiguration = true;
    requestUpdate();
    return;
  }
  const bool changed = candidate.url != connection.url || candidate.token != connection.token;
  if (changed && (store.pendingCount() != 0 || store.cardCount() != 0)) {
    auto confirmation = makeUniqueNoThrow<ConfirmationActivity>(renderer, mappedInput, tr(STR_ANKI_CONNECTION),
                                                                tr(STR_ANKI_SAME_COLLECTION));
    if (!confirmation) {
      LOG_ERR("ANKI", "Cannot allocate collection confirmation");
      saveError = true;
      requestUpdate();
      return;
    }
    startActivityForResult(std::move(confirmation),
                           [this, candidate = std::move(candidate)](const ActivityResult& result) {
                             if (result.isCancelled) return;
                             RenderLock lock(*this);
                             saveError = !anki::AnkiConnectionStore::save(candidate);
                             if (!saveError) connection = candidate;
                           });
    return;
  }
  saveError = !anki::AnkiConnectionStore::save(candidate);
  if (!saveError) connection = std::move(candidate);
  requestUpdate();
}

void AnkiSyncActivity::activateRow() {
  if (running.load()) return;
  invalidConfiguration = false;
  saveError = false;
  if (selectedIndex == 2) {
    launchSync();
    return;
  }
  const bool tokenField = selectedIndex == 1;
  auto keyboard = makeUniqueNoThrow<KeyboardEntryActivity>(
      renderer, mappedInput, tokenField ? tr(STR_ANKI_TOKEN) : tr(STR_ANKI_SERVER_URL),
      tokenField               ? connection.token
      : connection.url.empty() ? "http://"
                               : connection.url,
      tokenField ? anki::AnkiConnectionStore::MAX_TOKEN_BYTES : anki::AnkiConnectionStore::MAX_URL_BYTES,
      tokenField ? InputType::Password : InputType::Url);
  if (!keyboard) {
    LOG_ERR("ANKI", "Cannot allocate connection keyboard");
    saveError = true;
    requestUpdate();
    return;
  }
  startActivityForResult(std::move(keyboard), [this, tokenField](const ActivityResult& result) {
    if (result.isCancelled) return;
    RenderLock lock(*this);
    auto candidate = connection;
    const auto& text = std::get<KeyboardResult>(result.data).text;
    if (tokenField)
      candidate.token = text;
    else
      candidate.url = text == "http://" || text == "https://" ? "" : text;
    saveConnection(std::move(candidate));
  });
}

void AnkiSyncActivity::launchSync() {
  if (!connection.configured()) {
    outcome = anki::SyncResult::NotConfigured;
    hasOutcome = true;
    requestUpdate();
    return;
  }
  if (store.failed() || store.needsAttention()) {
    outcome = store.failed() ? anki::SyncResult::StorageError : anki::SyncResult::Attention;
    hasOutcome = true;
    requestUpdate();
    return;
  }
  if (configureOnly) {
    // Enter the same minimal-network boot path as Home -> Sync after editing
    // credentials; the full reader boot keeps more resources resident.
    activityManager.goToAnkiSync(false);
    return;
  }
  client = makeUniqueNoThrow<anki::AnkiSyncClient>();
  if (!client || !client->initialize(store)) {
    LOG_ERR("ANKI", "Cannot initialize sync client");
    client.reset();
    outcome = anki::SyncResult::StorageError;
    hasOutcome = true;
    requestUpdate();
    return;
  }
  networkTouched = true;
  hasOutcome = false;
  if (WiFi.status() == WL_CONNECTED) {
    startWorker();
    return;
  }
  auto wifi = makeUniqueNoThrow<WifiSelectionActivity>(renderer, mappedInput);
  if (!wifi) {
    LOG_ERR("ANKI", "Cannot allocate Wi-Fi selection");
    client.reset();
    outcome = anki::SyncResult::StorageError;
    hasOutcome = true;
    requestUpdate();
    return;
  }
  startActivityForResult(std::move(wifi), [this](const ActivityResult& result) {
    RenderLock lock(*this);
    if (result.isCancelled) {
      client.reset();
      outcome = anki::SyncResult::Cancelled;
      hasOutcome = true;
    } else {
      startWorker();
    }
  });
}

void AnkiSyncActivity::startWorker() {
  cancel = false;
  done = false;
  running = true;
  phase = anki::SyncPhase::Checking;
  displayedPhase = anki::SyncPhase::Checking;
  // The worker owns all card-store/network work; the UI reads only atomics.
  if (xTaskCreate(runWorker, "AnkiSync", WORKER_STACK_BYTES, this, 1, &worker) != pdPASS) {
    LOG_ERR("ANKI", "Cannot create sync worker");
    worker = nullptr;
    running = false;
    done = true;
    outcome = anki::SyncResult::StorageError;
    hasOutcome = true;
    client.reset();
  }
  requestUpdate();
}

void AnkiSyncActivity::runWorker(void* context) {
  auto* self = static_cast<AnkiSyncActivity*>(context);
  anki::SyncCallbacks callbacks;
  callbacks.context = self;
  callbacks.cancelled = [](void* context) { return static_cast<AnkiSyncActivity*>(context)->cancel.load(); };
  callbacks.phaseChanged = [](void* context, const anki::SyncPhase phase) {
    static_cast<AnkiSyncActivity*>(context)->phase.store(phase);
  };
  const auto result = self->client->run(self->connection, callbacks);
  LOG_INF("ANKI", "Sync finished: result=%u stackFree=%u", static_cast<unsigned>(result),
          static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
  self->outcome.store(result, std::memory_order_relaxed);
  // This is the worker's last access to activity-owned state. onExit waits for
  // this release before destroying the client, connection, or card store.
  self->done.store(true, std::memory_order_release);
#ifndef SIMULATOR
  vTaskDelete(nullptr);
#endif
}

const char* AnkiSyncActivity::statusText() const {
  if (running.load()) {
    switch (phase.load()) {
      case anki::SyncPhase::Checking:
        return tr(STR_ANKI_CHECKING);
      case anki::SyncPhase::Uploading:
        return tr(STR_ANKI_UPLOADING);
      case anki::SyncPhase::Downloading:
        return tr(STR_ANKI_DOWNLOADING);
    }
  }
  if (invalidConfiguration) return tr(STR_ANKI_INVALID_CONFIG);
  if (saveError) return tr(STR_ANKI_SAVE_FAILED);
  if (!hasOutcome) return tr(STR_ANKI_CONFIG_REQUIRED);
  switch (outcome.load()) {
    case anki::SyncResult::Success:
      return tr(STR_ANKI_SYNC_COMPLETE);
    case anki::SyncResult::Attention:
      return tr(STR_ANKI_RECOVERY_NEEDED);
    case anki::SyncResult::StorageError:
      return tr(STR_ANKI_STORAGE_ERROR);
    case anki::SyncResult::NotConfigured:
      return tr(STR_ANKI_CONFIG_REQUIRED);
    case anki::SyncResult::Cancelled:
      return tr(STR_CANCEL);
    default:
      return tr(STR_ANKI_SYNC_FAILED);
  }
}

void AnkiSyncActivity::loop() {
  if (running.load()) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
        TouchHeaderBackButton::wasTapped(mappedInput, TouchHeaderBackButton::compactHeaderRect(renderer))) {
      cancel = true;
      exitAfterWorker = true;
    }
    if (done.load(std::memory_order_acquire)) {
      joinWorker();
      RenderLock lock(*this);
      client.reset();
      hasOutcome = true;
      requestUpdate();
      if (exitAfterWorker) activityManager.goToAnki();
    } else if (displayedPhase != phase.load()) {
      displayedPhase = phase.load();
      requestUpdate();
    }
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
      TouchHeaderBackButton::wasTapped(mappedInput, TouchHeaderBackButton::compactHeaderRect(renderer))) {
    activityManager.goToAnki();
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
  navigator.onNextRelease([this] {
    selectedIndex = ButtonNavigator::nextIndex(selectedIndex, 3);
    requestUpdate();
  });
  navigator.onPreviousRelease([this] {
    selectedIndex = ButtonNavigator::previousIndex(selectedIndex, 3);
    requestUpdate();
  });
}

void AnkiSyncActivity::onRow(const fui::ActionEvent& event, void* context) {
  auto* self = static_cast<AnkiSyncActivity*>(context);
  if (event.value < 0 || event.value >= 3 || self->running.load()) return;
  self->selectedIndex = event.value;
  self->app.clearTapFlash();
  self->activateRow();
}

void AnkiSyncActivity::screen(UiApp::ScreenType& screen, void* context) {
  auto* self = static_cast<AnkiSyncActivity*>(context);
  const auto& metrics = UITheme::getInstance().getMetrics();
  screen.setContentMargin(fui::Insets{static_cast<int16_t>(CompactHeader::contentTop(metrics)), 0,
                                      static_cast<int16_t>(metrics.buttonHintsHeight), 0});
  const char* message = self->statusText();
  const auto bounds = screen.body().inset(fui::Insets{0, static_cast<int16_t>(metrics.contentSidePadding), 0,
                                                      static_cast<int16_t>(metrics.contentSidePadding)});
  const auto statusStyle = anki::ui::messageStyle(screen.target(), bounds, screen.theme().smallText);
  const auto size = fui::measureWrappedText(screen.target(), message, statusStyle, bounds.width);
  auto band = screen.takeTop(std::max(screen.theme().rowHeight, size.height), screen.theme().spaceSm);
  band.x = bounds.x;
  band.width = bounds.width;
  anki::ui::drawMessage(screen.target(), band, message, statusStyle);
  if (self->running.load()) return;
  fui::ListItem items[3];
  items[0].label = tr(STR_ANKI_SERVER_URL);
  items[0].value = self->connection.url.empty() ? tr(STR_NOT_SET) : self->connection.url.c_str();
  items[1].label = tr(STR_ANKI_TOKEN);
  items[1].value = self->connection.token.empty() ? tr(STR_NOT_SET) : "******";
  items[2].label = tr(STR_ANKI_SYNC_NOW);
  for (int i = 0; i < 3; ++i) items[i].actionValue = i;
  fui::ListProps props;
  props.items = items;
  props.count = 3;
  props.selectedIndex = self->selectedIndex;
  props.action = ROW_ACTION;
  props.inputMask = fui::InputTouch;
  props.labelText = screen.theme().bodyText;
  props.labelText.maxLines = 2;
  configureUiList(props, screen.theme(), screen.body());
  screen.list(props);
}

void AnkiSyncActivity::render(RenderLock&&) {
  renderer.clearScreen();
  TouchHeaderBackButton::drawCompact(renderer, configureOnly ? tr(STR_ANKI_CONNECTION) : tr(STR_ANKI_SYNC));
  app.setDevice(uiTarget.deviceContext());
  uiReady = false;
  app.render();
  uiReady = !running.load();
  const auto labels = mappedInput.mapLabels(mappedInput.withBackArrow(running.load() ? tr(STR_CANCEL) : tr(STR_BACK)),
                                            running.load() ? "" : tr(STR_OPEN), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
#endif
