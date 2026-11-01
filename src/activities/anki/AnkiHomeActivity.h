#pragma once

#include "AppCapabilities.h"

#if CROSSINK_APP_CAP_ANKI
#include <FreeInkApp.h>
#include <FreeInkUIGfxRenderer.h>

#include "activities/Activity.h"
#include "anki/AnkiStore.h"
#include "util/ButtonNavigator.h"

class AnkiHomeActivity final : public Activity {
 public:
  AnkiHomeActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool allowGlobalHomeSwipeGesture() const override { return false; }
#ifdef SIMULATOR
  void simulatorShowSaveError() { actionError = true; }
#endif

 private:
  using UiApp = freeink::ui::FreeInkApp<24, 2>;
  anki::AnkiStore store;
  // The SDK consumes each provider label before requesting the next row.
  anki::Deck rowDeck;
  char rowValue[24] = {};
  char rowSubtitle[128] = {};
  freeink::ui::GfxRendererTarget uiTarget;
  UiApp app;
  ButtonNavigator navigator;
  int selectedIndex = 0;
  int topIndex = 0;
  int visibleRows = 1;
  bool uiReady = false;
  bool storageError = false;
  bool actionError = false;
  bool ignoreConfirmRelease = false;

  int rowCount() const;
  int deckRowOffset() const;
  void reload();
  void activateRow();
  void openRecovery();
  static void screen(UiApp::ScreenType& screen, void* context);
  static void row(void* context, uint16_t index, freeink::ui::ListItem& item);
  static void onRow(const freeink::ui::ActionEvent& event, void* context);
};
#endif
