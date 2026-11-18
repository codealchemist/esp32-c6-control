#pragma once

#include <Arduino.h>
#include <lvgl.h>

#include <initializer_list>

#include "input.h"

// Content-driven screen layouts for the 320x172 landscape panel. A screen
// describes WHAT it shows — text, an optional icon, 0..3 actions — and the
// layout is chosen from that:
//
//   no actions, no icon   text fullscreen (scrolls if long)
//   no actions, icon      big icon in a left column, text fills the rest
//   1 action              tall button on the left, icon + text on the right
//   2-3 actions           text on top, actions in a 52px bar at the bottom as
//                         ONE segmented bar (a single rounded button split by
//                         thin black lines), so no touch area is lost to gaps
//
// Text that doesn't fit its area is cut with "..." and becomes tappable:
// a tap opens it fullscreen (scrollable) on top of the screen, another tap
// closes it. The screen's own loop keeps running underneath, unaware.
//
// Actions inject InputEvents (see inputInject), so a screen handles touch,
// encoder and button through one path.

struct UiAction {
  const char *label;  // text, or an LV_SYMBOL_* glyph
  InputEvent event;
};

struct UiView {
  lv_obj_t *screen = nullptr;
  lv_obj_t *content = nullptr;  // the text/content area, for custom children
  lv_obj_t *text = nullptr;     // main text label (null if text was null)
};

// Builds and loads a new screen (deleting the previous one). `text` may be
// nullptr for screens that fill `content` themselves. `name` labels the
// screen in the periodic [Diag] line.
UiView uiShow(const char *name, const char *text, const char *symbol = nullptr,
              lv_color_t symbolColor = lv_color_white(),
              std::initializer_list<UiAction> actions = {});

// A new, empty screen for the few views with bespoke layouts (the QR code,
// the connecting spinner). Deletes the previous screen like uiShow().
lv_obj_t *uiBlankScreen(const char *name);

// Replaces the main text, re-evaluating whether it overflows.
void uiSetText(const UiView &view, const String &text);

// Makes the whole content area a tap target sending `event` — on adjust
// screens, tapping the value area confirms ("set") alongside the -/+ bar.
void uiSetContentAction(const UiView &view, InputEvent event);

// Name of the screen currently shown, for diagnostics.
const char *uiScreenName();
