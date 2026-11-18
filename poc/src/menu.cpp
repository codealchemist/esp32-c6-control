#include "menu.h"

#include <ArduinoJson.h>

#include <functional>

#include "app.h"
#include "config_store.h"
#include "input.h"
#include "net.h"
#include "provisioning.h"
#include "ui_layout.h"

namespace {

constexpr uint32_t kIdleCancelMs = 5000;    // edits: no input for this long cancels
constexpr uint32_t kResultTimeoutMs = 5000; // §6.3: short client-side result timeout
constexpr uint32_t kToastMs = 1500;

// Waits up to `ms` for any input, pumping everything else. Returns the event,
// or None on timeout.
InputEvent waitAny(uint32_t ms) {
  const uint32_t start = millis();
  while (millis() - start < ms) {
    appPump();
    const InputEvent ev = inputPoll();
    if (ev != InputEvent::None) return ev;
    delay(5);
  }
  return InputEvent::None;
}

// A transient message: icon + text, gone after `ms` or on any input.
void toast(const char *name, const String &text, const char *symbol, lv_color_t color,
           uint32_t ms = kToastMs) {
  uiShow(name, text.c_str(), symbol, color);
  waitAny(ms);
}

// A message that waits for OK (one action: button left, text right).
void notice(const char *name, const String &text, const char *symbol, lv_color_t color) {
  uiShow(name, text.c_str(), symbol, color, {{"OK", InputEvent::EncoderPressShort}});
  while (waitAny(60000) == InputEvent::None) {
  }
}

// Two-action confirm for destructive actions. True only on the right-hand
// action; BOOT or 10s idle cancel.
bool confirm(const String &text, const char *yesLabel) {
  uiShow("confirm", text.c_str(), LV_SYMBOL_WARNING, lv_palette_main(LV_PALETTE_AMBER),
         {{"Cancel", InputEvent::ButtonPressShort}, {yesLabel, InputEvent::SecondaryAction}});
  return waitAny(10000) == InputEvent::SecondaryAction;
}

const char *nodeSymbol(JsonObjectConst n) {
  const String type = n["type"] | "";
  if (type == "folder") {
    return String(n["binding"] | "") == "message_inbox" ? LV_SYMBOL_ENVELOPE : LV_SYMBOL_DIRECTORY;
  }
  if (type == "action") return LV_SYMBOL_SETTINGS;
  return LV_SYMBOL_PLAY;  // task
}

String newRequestId() {
  char buf[12];
  snprintf(buf, sizeof(buf), "%08lx", static_cast<unsigned long>(esp_random()));
  return buf;
}

// --- Tasks (§6.3) ---------------------------------------------------------------

// Collects param values: encoderAdjustable ones through their editor, the
// rest at their default. False if the user cancels, or a param type has no
// editor yet — nothing is sent with partial params (§5.2.1).
bool collectParams(JsonObjectConst node, JsonObject out) {
  for (JsonObjectConst p : node["task"]["params"].as<JsonArrayConst>()) {
    const char *key = p["key"] | "";
    const String type = p["type"] | "";
    if (!(p["encoderAdjustable"] | false)) {
      out[key] = p["default"];
      continue;
    }
    if (type == "int") {
      int v = p["default"] | 0;
      if (!uiEditInt(String(node["label"] | "") + ": " + key, v, p["min"] | 0, p["max"] | 100,
                     p["step"] | 1)) {
        Serial.printf("[Menu] %s: edit of '%s' cancelled — nothing sent\n",
                      node["id"].as<const char *>(), key);
        return false;
      }
      out[key] = v;
    } else {
      notice("unsupported", "Param '" + String(key) + "'\ntype '" + type + "'\nnot supported yet",
             LV_SYMBOL_WARNING, lv_palette_main(LV_PALETTE_AMBER));
      return false;
    }
  }
  return true;
}

void runTask(JsonObjectConst node) {
  const char *id = node["id"] | "?";
  const char *label = node["label"] | id;
  JsonObjectConst task = node["task"];

  JsonDocument params;
  if (!collectParams(node, params.to<JsonObject>())) return;

  // §6.3: no queue/replay — a stale "lights on" sent on reconnect would be
  // worse than saying it didn't go out.
  if (!netMqttIsConnected()) {
    notice("task-offline", String(label) + "\nNot sent: offline", LV_SYMBOL_CLOSE,
           lv_palette_main(LV_PALETTE_RED));
    return;
  }

  // CoreEvent wire shape on the messages topic. `value` is the requestId so
  // every invocation is a state *change* for the Core's State Engine (a
  // repeated "Lights On" would otherwise be dropped as a heartbeat); the task
  // itself travels in `metadata`.
  const String requestId = newRequestId();
  const bool expectsResult = task["expectsResult"] | false;
  JsonDocument ev;
  ev["componentId"] = id;
  ev["eventType"] = "task";
  ev["value"] = requestId;
  JsonObject meta = ev["metadata"].to<JsonObject>();
  meta["taskId"] = task["payload"]["taskId"] | id;
  meta["expectsResult"] = expectsResult;
  JsonObject payload = meta["payload"].to<JsonObject>();
  for (JsonPairConst kv : task["payload"].as<JsonObjectConst>()) payload[kv.key()] = kv.value();
  for (JsonPairConst kv : params.as<JsonObjectConst>()) payload[kv.key()] = kv.value();
  payload["requestId"] = requestId;

  String json;
  serializeJson(ev, json);
  const bool sent = netMqttPublishEvent(json, task["qos"] | 1);
  Serial.println("[Task] " + String(sent ? "sent " : "NOT sent ") + json);
  if (!sent) {
    notice("task-failed", String(label) + "\nNot sent", LV_SYMBOL_CLOSE,
           lv_palette_main(LV_PALETTE_RED));
    return;
  }

  if (!expectsResult) {
    // Fire-and-forget: acknowledge locally right away, no waiting (§5.2).
    toast("task-sent", String(label) + "\nSent", LV_SYMBOL_OK, lv_palette_main(LV_PALETTE_GREEN));
    return;
  }

  uiShow("task-wait", (String(label) + "\nWaiting for result...").c_str(), LV_SYMBOL_REFRESH,
         lv_palette_main(LV_PALETTE_BLUE));
  const uint32_t start = millis();
  String resultJson;
  while (millis() - start < kResultTimeoutMs) {
    appPump();
    inputPoll();  // keep the input queue drained; waiting is not cancellable
    if (appTakeTaskResult(requestId, resultJson)) break;
    delay(5);
  }
  if (resultJson.isEmpty()) {
    Serial.println("[Task] " + requestId + ": no result within 5s");
    notice("task-timeout", String(label) + "\nNo result (timeout)", LV_SYMBOL_WARNING,
           lv_palette_main(LV_PALETTE_AMBER));
    return;
  }

  JsonDocument r;
  deserializeJson(r, resultJson);
  const bool ok = String(r["result"]["status"] | "ok") == "ok";
  const String message = r["result"]["message"] | (ok ? "Done" : "Failed");
  Serial.println("[Task] " + requestId + " result: " + message);
  notice("task-result", String(label) + "\n" + message, ok ? LV_SYMBOL_OK : LV_SYMBOL_CLOSE,
         lv_palette_main(ok ? LV_PALETTE_GREEN : LV_PALETTE_RED));
}

// --- Local actions (§5.2) -------------------------------------------------------

void runAction(JsonObjectConst node) {
  const String action = node["action"] | "";
  Serial.println("[Menu] action " + action);
  if (action == "showAbout") {
    notice("about", "About\nIcons by Icons8 - https://icons8.com", LV_SYMBOL_LIST,
           lv_color_white());
  } else if (action == "reprovision") {
    if (!confirm("Reset Wi-Fi/MQTT?\nPairing is erased", "Reset")) return;
    toast("reprovision", "Resetting...\nPair again after restart", LV_SYMBOL_REFRESH,
          lv_palette_main(LV_PALETTE_BLUE));
    provisioningForceReset();
    Serial.flush();
    ESP.restart();
  } else if (action == "restoreDefaultConfig") {
    if (!confirm("Restore default config?", "Restore")) return;
    configRestoreDefault();
    toast("restore-config", "Default restored\nRestarting...", LV_SYMBOL_REFRESH,
          lv_palette_main(LV_PALETTE_BLUE));
    Serial.flush();
    ESP.restart();
  } else {
    notice("unknown-action", "Unknown action\n" + action, LV_SYMBOL_WARNING,
           lv_palette_main(LV_PALETTE_AMBER));
  }
}

// --- Navigation (§6.2 NAVIGATE) ------------------------------------------------
//
// Two items per page, one per screen half: big icon on top, label below.
// Touch: tap an item to open it, swipe left/right to page, swipe down (or tap
// the "< Folder" header) to go back. Encoder: turn moves a highlighted
// selection item by item, push opens it. BOOT = back. The root level is the
// home screen: it never closes, and its header shows the connection state
// (tap it for details).

void openNode(JsonObjectConst node, const String &path);

constexpr int32_t kW = 320;
constexpr int32_t kH = 172;
constexpr int32_t kHeaderH = 30;
constexpr int32_t kCellM = 4;

// Touch state for the current grid screen. A swipe starts with a press on a
// cell, so LVGL may still report a click on release; a detected gesture
// marks the press as used up.
int gTappedCell = -1;
bool gGestured = false;

void onCellPressed(lv_event_t *) { gGestured = false; }

void onCellClicked(lv_event_t *e) {
  if (gGestured) return;
  gTappedCell = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
  inputInject(InputEvent::EncoderPressShort);
}

void onGesture(lv_event_t *) {
  const lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_active());
  gGestured = true;
  if (dir == LV_DIR_LEFT) inputInject(InputEvent::EncoderCW);         // next page
  else if (dir == LV_DIR_RIGHT) inputInject(InputEvent::EncoderCCW);  // previous page
  else if (dir == LV_DIR_BOTTOM) inputInject(InputEvent::ButtonPressShort);  // back
  else return;
  lv_indev_wait_release(lv_indev_active());
}

void onHeaderClicked(lv_event_t *e) {
  inputInject(static_cast<InputEvent>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e))));
}

String connectionText() { return netMqttIsConnected() ? "Online" : "Offline"; }

struct Grid {
  lv_obj_t *status = nullptr;  // root only: connection label
};

Grid drawGrid(JsonArrayConst nodes, size_t page, int selected, const String &title, bool root) {
  const size_t n = nodes.size();
  const size_t pages = (n + 1) / 2;
  Grid g;
  lv_obj_t *scr = uiBlankScreen(root ? "home" : "menu");
  lv_obj_add_event_cb(scr, onGesture, LV_EVENT_GESTURE, nullptr);

  // Header: connection state (root) or "< Folder" (back), page dots right.
  lv_obj_t *hdr = lv_obj_create(scr);
  lv_obj_remove_style_all(hdr);
  lv_obj_set_pos(hdr, 0, 0);
  lv_obj_set_size(hdr, 200, kHeaderH);
  lv_obj_add_flag(hdr, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_bg_color(hdr, lv_palette_main(LV_PALETTE_GREY), LV_STATE_PRESSED);
  lv_obj_set_style_bg_opa(hdr, LV_OPA_30, LV_STATE_PRESSED);
  lv_obj_set_style_radius(hdr, 8, 0);
  lv_obj_add_event_cb(hdr, onHeaderClicked, LV_EVENT_CLICKED,
                      reinterpret_cast<void *>(static_cast<intptr_t>(
                          root ? InputEvent::SecondaryAction : InputEvent::ButtonPressShort)));
  lv_obj_t *ht = lv_label_create(hdr);
  if (root) {
    const bool on = netMqttIsConnected();
    lv_label_set_text(ht, (String(LV_SYMBOL_WIFI "  ") + connectionText()).c_str());
    lv_obj_set_style_text_color(ht, lv_palette_main(on ? LV_PALETTE_GREEN : LV_PALETTE_RED), 0);
    g.status = ht;
  } else {
    lv_label_set_text(ht, (String(LV_SYMBOL_LEFT "  ") + title).c_str());
    lv_obj_set_width(ht, 190);
    lv_label_set_long_mode(ht, LV_LABEL_LONG_DOT);
  }
  lv_obj_align(ht, LV_ALIGN_LEFT_MID, 8, 0);

  if (pages > 1) {
    constexpr int32_t kDot = 7, kGap = 6;
    for (size_t i = 0; i < pages; i++) {
      lv_obj_t *d = lv_obj_create(scr);
      lv_obj_remove_style_all(d);
      lv_obj_set_size(d, kDot, kDot);
      lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
      lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
      lv_obj_set_style_bg_color(d, i == page ? lv_color_white() : lv_palette_darken(LV_PALETTE_GREY, 2), 0);
      lv_obj_set_pos(d, kW - 10 - (pages - i) * (kDot + kGap) + kGap, (kHeaderH - kDot) / 2);
    }
  }

  // The two cells.
  const int32_t cellW = kW / 2 - 2 * kCellM;
  const int32_t cellH = kH - kHeaderH - kCellM;
  for (size_t c = 0; c < 2; c++) {
    const size_t idx = page * 2 + c;
    if (idx >= n) break;
    JsonObjectConst node = nodes[idx];
    lv_obj_t *cell = lv_obj_create(scr);
    lv_obj_remove_style_all(cell);
    lv_obj_set_pos(cell, c * (kW / 2) + kCellM, kHeaderH);
    lv_obj_set_size(cell, cellW, cellH);
    lv_obj_remove_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(cell, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_radius(cell, 12, 0);
    lv_obj_set_style_bg_opa(cell, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(cell, lv_color_hex(0x1C1C1C), 0);
    lv_obj_set_style_bg_color(cell, lv_color_hex(0x3A3A3A), LV_STATE_PRESSED);
    if (static_cast<int>(idx) == selected) {
      lv_obj_set_style_border_width(cell, 2, 0);
      lv_obj_set_style_border_color(cell, lv_palette_main(LV_PALETTE_BLUE), 0);
    }
    lv_obj_add_event_cb(cell, onCellPressed, LV_EVENT_PRESSED, nullptr);
    lv_obj_add_event_cb(cell, onCellClicked, LV_EVENT_CLICKED,
                        reinterpret_cast<void *>(static_cast<intptr_t>(idx)));

    lv_obj_t *icon = lv_label_create(cell);
    lv_label_set_text(icon, nodeSymbol(node));
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(icon, lv_color_white(), 0);
    lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, 16);

    const String label = node["label"] | "?";
    lv_obj_t *name = lv_label_create(cell);
    lv_label_set_text(name, label.c_str());
    // Short labels at 28px; longer ones at 20px over up to two lines.
    const bool big = label.length() <= 9;
    lv_obj_set_style_text_font(name, big ? &lv_font_montserrat_28 : &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(name, cellW - 12);
    if (!big) {
      lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
      lv_obj_set_height(name, 2 * lv_font_get_line_height(&lv_font_montserrat_20));
    }
    lv_obj_align(name, LV_ALIGN_TOP_MID, 0, 16 + 48 + 10);
  }
  return g;
}

// Shows one level until the user backs out. The root level never returns.
void showLevel(JsonArrayConst nodes, const String &title, bool root,
               const std::function<String()> &statusText) {
  const size_t n = nodes.size();
  if (n == 0 && !root) {
    notice("menu-empty", title + "\n(empty)", LV_SYMBOL_DIRECTORY, lv_color_white());
    return;
  }

  size_t page = 0;
  int selected = -1;  // encoder highlight; touch use leaves it off
  const size_t pages = n ? (n + 1) / 2 : 1;
  while (true) {
    gTappedCell = -1;
    gGestured = false;
    Grid g = drawGrid(nodes, page, selected, title, root);
    if (n == 0) uiShow("home-empty", "No menu items\nin the active config", LV_SYMBOL_LIST);
    bool wasOnline = netMqttIsConnected();

    InputEvent ev = InputEvent::None;
    while (ev == InputEvent::None) {
      appPump();
      ev = inputPoll();
      if (root && g.status && netMqttIsConnected() != wasOnline) {
        wasOnline = !wasOnline;
        lv_label_set_text(g.status, (String(LV_SYMBOL_WIFI "  ") + connectionText()).c_str());
        lv_obj_set_style_text_color(
            g.status, lv_palette_main(wasOnline ? LV_PALETTE_GREEN : LV_PALETTE_RED), 0);
      }
      delay(5);
    }
    const bool touched = gTappedCell >= 0;
    switch (ev) {
      case InputEvent::EncoderCW:
      case InputEvent::EncoderCCW: {
        if (n == 0) break;
        const bool fwd = ev == InputEvent::EncoderCW;
        if (gGestured) {  // swipe: whole page
          page = fwd ? (page + 1) % pages : (page + pages - 1) % pages;
          selected = -1;
          Serial.printf("[Menu] swipe -> page %u/%u\n", page + 1, pages);
        } else {          // encoder: item by item
          selected = selected < 0 ? static_cast<int>(page * 2)
                                  : (fwd ? (selected + 1) % n : (selected + n - 1) % n);
          page = selected / 2;
        }
        break;
      }
      case InputEvent::EncoderPressShort: {
        if (n == 0) break;
        const int idx = touched ? gTappedCell : (selected >= 0 ? selected : page * 2);
        if (!touched) selected = idx;
        openNode(nodes[idx], title);  // then redraw this level
        break;
      }
      case InputEvent::ButtonPressShort:
      case InputEvent::ButtonPressLong:
        if (!root) return;
        page = 0;  // home: back = first page
        selected = -1;
        break;
      case InputEvent::SecondaryAction:
        if (root && statusText) {
          notice("status", connectionText() + "\n" + statusText(),
                 netMqttIsConnected() ? LV_SYMBOL_WIFI : LV_SYMBOL_WARNING,
                 lv_palette_main(netMqttIsConnected() ? LV_PALETTE_GREEN : LV_PALETTE_RED));
        }
        break;
      default:
        break;
    }
  }
}

void openNode(JsonObjectConst node, const String &path) {
  const String type = node["type"] | "";
  const String label = node["label"] | "";
  Serial.println("[Menu] open " + String(node["id"] | "?") + " (" + type + ")");
  if (type == "task") {
    runTask(node);
  } else if (type == "action") {
    runAction(node);
  } else if (type == "folder") {
    if (String(node["binding"] | "") == "message_inbox") {
      notice("inbox", label + "\nNo messages\n(inbox not built yet)", LV_SYMBOL_ENVELOPE,
             lv_color_white());
    } else {
      showLevel(node["children"].as<JsonArrayConst>(), label, false, nullptr);
    }
  }
}

} // namespace

void menuHome(std::function<String()> statusText) {
  Serial.println("[Menu] home: tap an item to open, swipe left/right to page, swipe down or "
                 "tap the header to go back; encoder +/- selects, 's' opens, 'b' back, "
                 "'a' shows status");
  showLevel(configMenu(), "Menu", true, statusText);
}

bool uiEditInt(const String &title, int &value, int minValue, int maxValue, int step,
               int initialSteps, std::function<String()> footer) {
  UiView v = uiShow("edit-int", nullptr, nullptr, lv_color_white(),
                    {{LV_SYMBOL_MINUS, InputEvent::EncoderCCW}, {LV_SYMBOL_PLUS, InputEvent::EncoderCW}});
  uiSetContentAction(v, InputEvent::EncoderPressShort);

  lv_obj_t *titleL = lv_label_create(v.content);
  lv_label_set_text(titleL, title.c_str());
  lv_obj_set_width(titleL, LV_PCT(100));
  lv_label_set_long_mode(titleL, LV_LABEL_LONG_DOT);
  lv_obj_align(titleL, LV_ALIGN_TOP_LEFT, 6, 4);

  lv_obj_t *hint = lv_label_create(v.content);
  lv_obj_set_style_text_color(hint, lv_palette_main(LV_PALETTE_GREY), 0);

  lv_obj_t *valueL = lv_label_create(v.content);
  lv_obj_set_style_text_font(valueL, &lv_font_montserrat_48, 0);

  auto clampv = [&](int x) { return x < minValue ? minValue : (x > maxValue ? maxValue : x); };
  int pending = clampv(value + initialSteps * step);
  auto refresh = [&]() {
    const bool dirty = pending != value;
    lv_label_set_text(valueL, String(pending).c_str());
    lv_obj_set_style_text_color(valueL, dirty ? lv_palette_main(LV_PALETTE_AMBER) : lv_color_white(),
                                0);
    lv_obj_align(valueL, LV_ALIGN_CENTER, 0, -6);
    lv_label_set_text(hint, dirty ? "Tap here to set" : (footer ? footer() : String("Tap to keep")).c_str());
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -2);
  };
  refresh();
  Serial.printf("[Edit] %s: -/+ adjust (%d..%d), tap value or encoder push sets, BOOT or %us "
                "idle cancels\n",
                title.c_str(), minValue, maxValue, kIdleCancelMs / 1000);

  uint32_t lastActivity = millis();
  while (true) {
    appPump();
    int steps = 0;
    const InputEvent ev = inputPoll();
    if (ev == InputEvent::EncoderCW) {
      steps = 1;
    } else if (ev == InputEvent::EncoderCCW) {
      steps = -1;
    } else if (ev == InputEvent::EncoderPressShort) {
      const bool changed = pending != value;
      value = pending;
      Serial.printf("[Edit] %s: set %d%s\n", title.c_str(), value, changed ? "" : " (unchanged)");
      return true;
    } else if (ev != InputEvent::None) {
      Serial.printf("[Edit] %s: cancelled (pending %d discarded)\n", title.c_str(), pending);
      return false;
    }

    if (steps != 0) {
      pending = clampv(pending + steps * step);
      lastActivity = millis();
      refresh();
    } else if (footer && pending == value) {
      // The footer may show live data (e.g. the Core's last echo); redraw only
      // when it actually changes, not every loop.
      static String lastFooter;
      const String f = footer();
      if (f != lastFooter) {
        lastFooter = f;
        refresh();
      }
    }

    if (millis() - lastActivity >= kIdleCancelMs &&
        lv_display_get_inactive_time(nullptr) >= kIdleCancelMs) {
      Serial.printf("[Edit] %s: idle — cancelled%s\n", title.c_str(),
                    pending != value ? (" (pending " + String(pending) + " discarded)").c_str() : "");
      return false;
    }
    delay(5);
  }
}
