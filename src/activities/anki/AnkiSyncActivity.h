#pragma once

#include "AppCapabilities.h"
#if CROSSINK_APP_CAP_ANKI
#include <FreeInkApp.h>
#include <FreeInkUIGfxRenderer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <atomic>
#include <memory>

#include "activities/Activity.h"
#include "anki/AnkiConnectionStore.h"
#include "anki/AnkiStore.h"
#include "anki/AnkiSyncClient.h"
#include "util/ButtonNavigator.h"

class AnkiSyncActivity final : public Activity {
 public:
  AnkiSyncActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, bool configureOnly = false);
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return true; }
  bool allowGlobalHomeGesture() const override { return !running.load(); }
  bool allowGlobalHomeSwipeGesture() const override { return false; }
#ifdef SIMULATOR
  struct SimulatorState {
    bool running;
    bool hasOutcome;
    bool configureOnly;
    anki::SyncResult outcome;
  };
  SimulatorState simulatorState() const { return {running.load(), hasOutcome, configureOnly, outcome.load()}; }
  // Exercise the real credential-save flow without keyboard input plumbing.
  void simulatorSaveConnection(const anki::Connection& candidate) { saveConnection(candidate); }
#endif

 private:
  using UiApp = freeink::ui::FreeInkApp<8, 2>;
  anki::AnkiStore store;
  anki::Connection connection;
  std::unique_ptr<anki::AnkiSyncClient> client;
  freeink::ui::GfxRendererTarget uiTarget;
  UiApp app;
  ButtonNavigator navigator;
  TaskHandle_t worker = nullptr;
  std::atomic<bool> running{false};
  std::atomic<bool> done{false};
  std::atomic<bool> cancel{false};
  std::atomic<anki::SyncPhase> phase{anki::SyncPhase::Checking};
  std::atomic<anki::SyncResult> outcome{anki::SyncResult::Cancelled};
  anki::SyncPhase displayedPhase = anki::SyncPhase::Checking;
  bool configureOnly;
  bool networkTouched = false;
  bool hasOutcome = false;
  bool invalidConfiguration = false;
  bool saveError = false;
  bool uiReady = false;
  bool exitAfterWorker = false;
  bool ignoreConfirmRelease = false;
  int selectedIndex = 0;

  void activateRow();
  void saveConnection(anki::Connection candidate);
  void launchSync();
  void startWorker();
  void joinWorker();
  const char* statusText() const;
  static void runWorker(void* context);
  static void screen(UiApp::ScreenType& screen, void* context);
  static void onRow(const freeink::ui::ActionEvent& event, void* context);
};
#endif
