#include "ui_layout.h"

#include <vector>

namespace {

constexpr int32_t kW = 320;
constexpr int32_t kH = 172;
constexpr int32_t kM = 6;           // outer margin
constexpr int32_t kIconColW = 88;   // minimum icon column, no-action layout
constexpr int32_t kIconPad = 12;    // space between an icon and the text beside/below it
constexpr int32_t kSideBtnW = 118;  // the single action button, 1-action layout
// 52px: well above the ~44px usual minimum for a finger target, while
// leaving two thirds of the screen to the content above.
constexpr int32_t kBarH = 52;
constexpr int32_t kBarY = kH - kM - kBarH;

constexpr uint32_t kSlideMs = 220;  // page/folder slide

const char *gScreenName = "boot";

// LV_SYMBOL_* glyphs are 3-byte UTF-8 sequences in the private-use area,
// all starting 0xEF; plain labels never do.
bool isSymbol(const char *s) { return s && static_cast<uint8_t>(s[0]) == 0xEF; }

// Per-label state for overflow handling. The label's own text can't be used
// to recover the full string: LONG_DOT rewrites it with the ellipsis.
struct TextInfo {
  String full;
  int32_t width;  // label width, fixed by the layout
  int32_t maxH;   // height available in the layout; 0 = area scrolls
};

void closeOverlay(lv_event_t *e) {
  // Deleting the object whose event is being handled must be deferred.
  lv_obj_delete_async(static_cast<lv_obj_t *>(lv_event_get_current_target(e)));
}

void openOverlay(lv_event_t *e) {
  auto *label = static_cast<lv_obj_t *>(lv_event_get_current_target(e));
  auto *info = static_cast<TextInfo *>(lv_obj_get_user_data(label));
  if (!info) return;

  lv_obj_t *overlay = lv_obj_create(lv_obj_get_screen(label));
  lv_obj_set_size(overlay, kW, kH);
  lv_obj_set_pos(overlay, 0, 0);
  lv_obj_set_style_radius(overlay, 0, 0);
  lv_obj_set_style_border_width(overlay, 0, 0);
  lv_obj_set_style_pad_all(overlay, 8, 0);
  lv_obj_set_scroll_dir(overlay, LV_DIR_VER);
  lv_obj_add_flag(overlay, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(overlay, closeOverlay, LV_EVENT_CLICKED, nullptr);

  lv_obj_t *t = lv_label_create(overlay);
  lv_obj_set_width(t, LV_PCT(100));
  lv_label_set_long_mode(t, LV_LABEL_LONG_WRAP);
  lv_label_set_text(t, (info->full + "\n\n" LV_SYMBOL_LEFT " tap to go back").c_str());
  // Taps on the text must reach the overlay's handler.
  lv_obj_add_flag(t, LV_OBJ_FLAG_EVENT_BUBBLE);
  Serial.println("[UI] text expanded fullscreen");
}

lv_obj_t *makeScreen(const char *name, lv_screen_load_anim_t anim = LV_SCREEN_LOAD_ANIM_NONE) {
  lv_obj_t *scr = lv_obj_create(nullptr);
  lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
  // auto_del: this app swaps screens forever, so the previous one must go —
  // LVGL deletes it after the slide (or right away without one), and if a
  // new screen arrives mid-slide it finishes that one first.
  lv_screen_load_anim(scr, anim, anim == LV_SCREEN_LOAD_ANIM_NONE ? 0 : kSlideMs, 0, true);
  gScreenName = name;
  return scr;
}

lv_obj_t *makeArea(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, int32_t h) {
  lv_obj_t *a = lv_obj_create(parent);
  lv_obj_remove_style_all(a);  // a plain box: no background, border or padding
  lv_obj_set_pos(a, x, y);
  lv_obj_set_size(a, w, h);
  lv_obj_remove_flag(a, LV_OBJ_FLAG_SCROLLABLE);
  // Sizes are applied lazily; make get_width/get_height valid right away.
  lv_obj_update_layout(a);
  return a;
}

lv_obj_t *makeSymbol(lv_obj_t *parent, const char *symbol, lv_color_t color, const lv_font_t *font) {
  lv_obj_t *icon = lv_label_create(parent);
  lv_label_set_text(icon, symbol);
  lv_obj_set_style_text_font(icon, font, 0);
  lv_obj_set_style_text_color(icon, color, 0);
  // Measured, not assumed: glyph widths differ (wifi is wider than warning),
  // and text is placed a fixed kIconPad after the icon's real edge.
  lv_obj_update_layout(icon);
  return icon;
}

void injectFromUserData(lv_event_t *e) {
  inputInject(static_cast<InputEvent>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e))));
}

// One tall button for the 1-action layout.
void makeSideButton(lv_obj_t *scr, const UiAction &a) {
  lv_obj_t *btn = lv_button_create(scr);
  lv_obj_set_pos(btn, kM, kM);
  lv_obj_set_size(btn, kSideBtnW, kH - 2 * kM);
  lv_obj_t *label = lv_label_create(btn);
  lv_label_set_text(label, a.label);
  if (isSymbol(a.label)) lv_obj_set_style_text_font(label, &lv_font_montserrat_48, 0);
  lv_obj_center(label);
  lv_obj_add_event_cb(btn, injectFromUserData, LV_EVENT_CLICKED,
                      reinterpret_cast<void *>(static_cast<intptr_t>(a.event)));
}

// 2-3 actions as ONE segmented control: a button matrix with no gaps, whose
// items are divided by lines (LV_BORDER_SIDE_INTERNAL draws borders only
// between items).
struct BarData {
  const char *map[4];
  InputEvent events[3];
};

void makeSegmentedBar(lv_obj_t *scr, std::initializer_list<UiAction> actions) {
  auto *data = new BarData{};
  size_t n = 0;
  bool allSymbols = true;
  for (const UiAction &a : actions) {
    if (n == 3) break;
    data->map[n] = a.label;
    data->events[n] = a.event;
    allSymbols = allSymbols && isSymbol(a.label);
    n++;
  }
  data->map[n] = "";  // terminator

  lv_obj_t *bar = lv_buttonmatrix_create(scr);
  lv_buttonmatrix_set_map(bar, data->map);
  lv_obj_set_pos(bar, kM, kBarY);
  lv_obj_set_size(bar, kW - 2 * kM, kBarH);

  lv_obj_set_style_pad_all(bar, 0, 0);
  lv_obj_set_style_pad_gap(bar, 0, 0);
  lv_obj_set_style_radius(bar, 12, 0);
  lv_obj_set_style_clip_corner(bar, true, 0);
  lv_obj_set_style_border_width(bar, 0, 0);

  lv_obj_set_style_radius(bar, 0, LV_PART_ITEMS);
  lv_obj_set_style_shadow_width(bar, 0, LV_PART_ITEMS);
  lv_obj_set_style_bg_color(bar, lv_palette_main(LV_PALETTE_BLUE), LV_PART_ITEMS);
  lv_obj_set_style_bg_color(bar, lv_palette_darken(LV_PALETTE_BLUE, 3),
                            LV_PART_ITEMS | LV_STATE_PRESSED);
  lv_obj_set_style_text_color(bar, lv_color_white(), LV_PART_ITEMS);
  lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_INTERNAL, LV_PART_ITEMS);
  // A thin black line between segments.
  lv_obj_set_style_border_width(bar, 1, LV_PART_ITEMS);
  lv_obj_set_style_border_color(bar, lv_color_black(), LV_PART_ITEMS);
  lv_obj_set_style_border_opa(bar, LV_OPA_COVER, LV_PART_ITEMS);
  // 48px glyphs no longer fit the 52px bar; 28px still reads as an icon.
  lv_obj_set_style_text_font(bar, allSymbols ? &lv_font_montserrat_28 : &lv_font_montserrat_20,
                             LV_PART_ITEMS);

  lv_obj_set_user_data(bar, data);
  lv_obj_add_event_cb(
      bar,
      [](lv_event_t *e) {
        auto *b = static_cast<lv_obj_t *>(lv_event_get_current_target(e));
        auto *d = static_cast<BarData *>(lv_obj_get_user_data(b));
        const uint32_t id = lv_buttonmatrix_get_selected_button(b);
        if (d && id < 3 && d->map[id][0] != '\0') inputInject(d->events[id]);
      },
      LV_EVENT_VALUE_CHANGED, nullptr);
  lv_obj_add_event_cb(
      bar,
      [](lv_event_t *e) {
        auto *b = static_cast<lv_obj_t *>(lv_event_get_current_target(e));
        delete static_cast<BarData *>(lv_obj_get_user_data(b));
      },
      LV_EVENT_DELETE, nullptr);
}

// Main text label in `area`. `scrolls`: the area scrolls instead of the text
// being cut (fullscreen text layout).
lv_obj_t *makeText(lv_obj_t *area, int32_t y, bool scrolls) {
  lv_obj_t *label = lv_label_create(area);
  lv_obj_set_pos(label, 0, y);
  const int32_t width = lv_obj_get_width(area);
  lv_obj_set_width(label, width);
  lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);

  auto *info = new TextInfo{};
  info->width = width;
  info->maxH = scrolls ? 0 : lv_obj_get_height(area) - y;
  lv_obj_set_user_data(label, info);
  lv_obj_add_event_cb(
      label,
      [](lv_event_t *e) {
        auto *l = static_cast<lv_obj_t *>(lv_event_get_current_target(e));
        delete static_cast<TextInfo *>(lv_obj_get_user_data(l));
      },
      LV_EVENT_DELETE, nullptr);
  return label;
}

} // namespace

lv_obj_t *uiBlankScreen(const char *name, lv_screen_load_anim_t anim) {
  return makeScreen(name, anim);
}

void uiSetText(const UiView &view, const String &text) {
  lv_obj_t *label = view.text;
  if (!label) return;
  auto *info = static_cast<TextInfo *>(lv_obj_get_user_data(label));
  if (info) info->full = text;

  if (!info || info->maxH == 0) {  // scrolling area: never cut
    lv_label_set_text(label, text.c_str());
    return;
  }

  // Does it fit? Measure with the label's actual font and width.
  lv_point_t size;
  const lv_font_t *font = lv_obj_get_style_text_font(label, LV_PART_MAIN);
  lv_text_get_size(&size, text.c_str(), font, 0, lv_obj_get_style_text_line_space(label, LV_PART_MAIN),
                   info->width, LV_TEXT_FLAG_NONE);
  const bool overflows = size.y > info->maxH;

  // Cut to whole lines, then let LONG_DOT add the ellipsis.
  const int32_t lineH = lv_font_get_line_height(font);
  lv_obj_set_height(label, overflows ? (info->maxH / lineH) * lineH : LV_SIZE_CONTENT);
  lv_label_set_long_mode(label, overflows ? LV_LABEL_LONG_DOT : LV_LABEL_LONG_WRAP);
  lv_label_set_text(label, text.c_str());

  // Tappable only when there is more to see.
  lv_obj_remove_event_cb(label, openOverlay);
  if (overflows) {
    lv_obj_add_flag(label, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(label, openOverlay, LV_EVENT_CLICKED, nullptr);
  } else {
    lv_obj_remove_flag(label, LV_OBJ_FLAG_CLICKABLE);
  }
}

UiView uiShow(const char *name, const char *text, const char *symbol, lv_color_t symbolColor,
              std::initializer_list<UiAction> actions) {
  UiView v;
  v.screen = makeScreen(name);
  const size_t n = actions.size();

  int32_t textY = 0;
  bool scrolls = false;

  if (n == 0 && !symbol) {
    // Text only: the whole screen, scrolling if needed.
    v.content = makeArea(v.screen, kM + 2, kM, kW - 2 * kM - 4, kH - 2 * kM);
    lv_obj_add_flag(v.content, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(v.content, LV_DIR_VER);
    scrolls = true;
  } else if (n == 0) {
    // Big icon left, text right.
    lv_obj_t *icon = makeSymbol(v.screen, symbol, symbolColor, &lv_font_montserrat_48);
    const int32_t col = max(kIconColW, lv_obj_get_width(icon) + 2 * kIconPad);
    lv_obj_align(icon, LV_ALIGN_LEFT_MID, (col - lv_obj_get_width(icon)) / 2, 0);
    v.content = makeArea(v.screen, col, kM, kW - col - kM, kH - 2 * kM);
  } else if (n == 1) {
    // Tall button left; icon above text on the right.
    makeSideButton(v.screen, *actions.begin());
    const int32_t x = kM + kSideBtnW + kIconPad;
    v.content = makeArea(v.screen, x, kM, kW - x - kM, kH - 2 * kM);
    if (symbol) {
      lv_obj_t *icon = makeSymbol(v.content, symbol, symbolColor, &lv_font_montserrat_48);
      textY = lv_obj_get_height(icon) + kIconPad / 2;
    }
  } else {
    // Text on top; segmented action bar at the bottom.
    int32_t x = kM + 4;
    if (symbol) {
      lv_obj_t *icon = makeSymbol(v.screen, symbol, symbolColor, &lv_font_montserrat_28);
      lv_obj_set_pos(icon, kM + 4, kM + 2);
      x = kM + 4 + lv_obj_get_width(icon) + kIconPad;
    }
    v.content = makeArea(v.screen, x, kM, kW - x - kM, kBarY - kM - 4);
    makeSegmentedBar(v.screen, actions);
  }

  if (text) {
    v.text = makeText(v.content, textY, scrolls);
    uiSetText(v, text);
  }
  return v;
}

void uiSetContentAction(const UiView &view, InputEvent event) {
  lv_obj_t *c = view.content;
  if (!c) return;
  // Labels are not clickable, so taps anywhere in the area land here.
  lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(c, injectFromUserData, LV_EVENT_CLICKED,
                      reinterpret_cast<void *>(static_cast<intptr_t>(event)));
  // Press feedback, so the tap visibly registers.
  lv_obj_set_style_radius(c, 10, 0);
  lv_obj_set_style_bg_color(c, lv_palette_main(LV_PALETTE_GREY), LV_STATE_PRESSED);
  lv_obj_set_style_bg_opa(c, LV_OPA_30, LV_STATE_PRESSED);
}

const char *uiScreenName() { return gScreenName; }
