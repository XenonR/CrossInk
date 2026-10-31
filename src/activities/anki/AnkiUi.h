#pragma once

#include <FreeInkUICore.h>

#include <algorithm>

namespace anki::ui {

inline freeink::ui::TextStyle messageStyle(const freeink::ui::DrawTarget& target, const freeink::ui::Rect bounds,
                                           freeink::ui::TextStyle style) {
  const int lineHeight = std::max(1, static_cast<int>(target.lineHeight(style.font)));
  style.maxLines = static_cast<uint8_t>(std::clamp(bounds.height / lineHeight, 1, 16));
  return style;
}

inline void drawMessage(freeink::ui::DrawTarget& target, const freeink::ui::Rect bounds, const char* message,
                        freeink::ui::TextStyle style) {
  if (bounds.height < target.lineHeight(style.font)) return;
  style = messageStyle(target, bounds, style);
  auto lineStyle = style;
  lineStyle.maxLines = 1;
  lineStyle.align = freeink::ui::TextAlign::Left;
  // Use the same bounded SDK layout for measurement and drawing, including
  // explicit newlines. Line spans use SDK scratch storage, with no heap copies.
  freeink::ui::layoutText(target, bounds, message, style,
                          [&](const char* line, const freeink::ui::Rect rect) { target.text(rect, line, lineStyle); });
}

}  // namespace anki::ui
