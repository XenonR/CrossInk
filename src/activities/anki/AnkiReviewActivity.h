#pragma once

#include "AppCapabilities.h"

#if CROSSINK_APP_CAP_ANKI
#include <FreeInkApp.h>
#include <FreeInkUIGfxRenderer.h>

#include <array>
#include <memory>

#include "activities/Activity.h"
#include "activities/ScreenTransitionRefresh.h"
#include "anki/AnkiStore.h"
#include "anki/AnkiTextLayout.h"

class AnkiReviewActivity final : public Activity {
 public:
  AnkiReviewActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, int64_t deckId);
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool allowGlobalHomeSwipeGesture() const override { return false; }
#ifdef SIMULATOR
  struct SimulatorState {
    uint16_t cardOrdinal;
    unsigned int pageNumber;
    bool answer;
    bool selectingRating;
    bool done;
    bool error;
    bool hasMore;
  };
  SimulatorState simulatorState() const {
    return {card.ordinal, pageNumber, answer, selectingRating, done, storageError || saveError, page.hasMore};
  }
  freeink::ui::Rect simulatorActionRect(int action) const { return actionRects[action]; }
  bool simulatorPageHasBold() const {
    bool found = false;
    for (size_t i = 0; i < page.lineCount; ++i) {
      anki::text::visitRuns(side.get(), lines[i], {&found, [](void* context, const char*, size_t, bool bold) {
                                                     if (bold) *static_cast<bool*>(context) = true;
                                                   }});
    }
    return found;
  }
#endif

 private:
  using UiApp = freeink::ui::FreeInkApp<12, 2>;
  static constexpr size_t kLineCapacity = 64;
  const int64_t deckId;
  anki::AnkiStore store;
  anki::CardInfo card;
  // One side buffer is reused for every face/card; its 16 KiB budget is too
  // large for a task stack and is released before the activity leaves.
  std::unique_ptr<char[]> side;
  // Offsets into the side, not copied strings. Excess lines go to the next page.
  std::array<anki::text::Line, kLineCapacity> lines{};
  anki::text::Cursor pageStart;
  anki::text::Page page;
  size_t sideLength = 0;
  unsigned int pageNumber = 0;
  int textWidth = 0;
  int textHeight = 0;
  size_t rowsPerPage = 1;
  int textFontId = 0;
  int drawX = 0;
  int drawY = 0;
  unsigned long cardStartedMs = 0;
  int focusedAction = 2;
  bool answer = false;
  bool selectingRating = false;
  bool done = false;
  bool storageError = false;
  bool saveError = false;
  bool uiReady = false;
  bool ignoreConfirmRelease = false;
  freeink::ui::GfxRendererTarget uiTarget;
  UiApp app;
#ifdef SIMULATOR
  std::array<freeink::ui::Rect, 8> actionRects{};
#endif
  ScreenTransitionRefresh refresh;

  void loadNextCard(uint16_t start);
  void loadSide();
  void activateAction(int action);
  bool actionEnabled(int action) const;
  void moveFocus(int delta);
  void changePage(bool next);
  void drawCard(UiApp::ScreenType& screen);
  void drawAction(UiApp::ScreenType& screen, freeink::ui::Rect rect, int action, const char* label);
  static void screen(UiApp::ScreenType& screen, void* context);
  static void onAction(const freeink::ui::ActionEvent& event, void* context);
  static int measure(void* context, const char* text, size_t length, bool bold);
  static void drawRun(void* context, const char* text, size_t length, bool bold);
};
#endif
