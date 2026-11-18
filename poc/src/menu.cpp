#include "menu.h"

#include <ArduinoJson.h>
#include <Preferences.h>

#include <functional>

#include "app.h"
#include "config_store.h"
#include "input.h"
#include "generated/icon_registry_gen.h"
#include "net.h"
#include "phone_hid.h"
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
  if (type == "switch") return LV_SYMBOL_POWER;
  if (type == "action") {
    return String(n["action"] | "") == "phoneVolume" ? LV_SYMBOL_VOLUME_MAX : LV_SYMBOL_SETTINGS;
  }
  return LV_SYMBOL_PLAY;  // task
}

const lv_image_dsc_t *iconImage(const char *id) {
  if (!id || !*id) return nullptr;
  for (size_t i = 0; i < kIconRegistryCount; i++) {
    if (strcmp(kIconRegistry[i].id, id) == 0) return kIconRegistry[i].img;
  }
  return nullptr;
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

// Publishes a task invocation (§6.3) — for `task` nodes and for `switch`
// nodes alike: `spec` is the node's task/switch object, `extra` is merged
// over spec.payload (params, or the switch's new state). CoreEvent wire shape
// on the messages topic: `value` is the requestId so every invocation is a
// state *change* for the Core's State Engine (a repeated "Lights On" would
// otherwise be dropped as a heartbeat); the task travels in `metadata`.
bool publishTask(JsonObjectConst node, JsonObjectConst spec, JsonObjectConst extra,
                 String &requestId) {
  const char *id = node["id"] | "?";
  requestId = newRequestId();
  JsonDocument ev;
  ev["componentId"] = id;
  ev["eventType"] = "task";
  ev["value"] = requestId;
  JsonObject meta = ev["metadata"].to<JsonObject>();
  meta["taskId"] = spec["payload"]["taskId"] | id;
  meta["expectsResult"] = spec["expectsResult"] | false;
  JsonObject payload = meta["payload"].to<JsonObject>();
  for (JsonPairConst kv : spec["payload"].as<JsonObjectConst>()) payload[kv.key()] = kv.value();
  for (JsonPairConst kv : extra) payload[kv.key()] = kv.value();
  payload["requestId"] = requestId;

  String json;
  serializeJson(ev, json);
  const bool sent = netMqttPublishEvent(json, spec["qos"] | 1);
  Serial.println("[Task] " + String(sent ? "sent " : "NOT sent ") + json);
  return sent;
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

  String requestId;
  const bool expectsResult = task["expectsResult"] | false;
  if (!publishTask(node, task, params.as<JsonObjectConst>(), requestId)) {
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

// --- Switches (§5.2 `switch`) ------------------------------------------------------
//
// A two-state button: each tap flips it and publishes the new state as a
// task — `payload` + `state: "on"|"off"` + that state's own object (`on` /
// `off` in the config, e.g. a different Tuya scene per state). The state is
// the panel's own (kept in NVS across reboots) and shown right away; with
// expectsResult, a failed or missing reply flips it back.

constexpr uint32_t kSwitchReplyMs = 5000;

String switchKey(const char *id) { return String(id).substring(0, 15); }  // NVS key limit

bool switchState(JsonObjectConst node) {
  Preferences p;
  p.begin("switches", true);
  const bool on = p.getBool(switchKey(node["id"] | "?").c_str(), node["switch"]["default"] | false);
  p.end();
  return on;
}

void setSwitchState(const char *id, bool on) {
  Preferences p;
  p.begin("switches", false);
  p.putBool(switchKey(id).c_str(), on);
  p.end();
}

struct PendingSwitch {
  bool active = false;
  String requestId, id, label;
  bool previous = false;
  uint32_t deadline = 0;
} gPendingSwitch;

void toggleSwitch(JsonObjectConst node) {
  const char *id = node["id"] | "?";
  const String label = node["label"] | id;
  JsonObjectConst spec = node["switch"];
  if (gPendingSwitch.active) {
    Serial.printf("[Switch] %s: still waiting for the last reply — ignored\n", gPendingSwitch.id.c_str());
    return;
  }
  if (!netMqttIsConnected()) {
    notice("switch-offline", label + "\nNot switched: offline", LV_SYMBOL_CLOSE,
           lv_palette_main(LV_PALETTE_RED));
    return;
  }
  const bool was = switchState(node);
  const bool on = !was;
  JsonDocument extra;
  extra["state"] = on ? "on" : "off";
  for (JsonPairConst kv : spec[on ? "on" : "off"].as<JsonObjectConst>()) extra[kv.key()] = kv.value();

  String requestId;
  if (!publishTask(node, spec, extra.as<JsonObjectConst>(), requestId)) {
    notice("switch-failed", label + "\nNot sent", LV_SYMBOL_CLOSE, lv_palette_main(LV_PALETTE_RED));
    return;
  }
  setSwitchState(id, on);
  Serial.printf("[Switch] %s -> %s\n", id, on ? "on" : "off");
  if (spec["expectsResult"] | false) {
    gPendingSwitch = {true, requestId, id, label, was, millis() + kSwitchReplyMs};
  }
}

// Called from the menu loop. True if the screen must be redrawn (a reply
// undid the switch).
bool pollSwitchReply() {
  if (!gPendingSwitch.active) return false;
  PendingSwitch &p = gPendingSwitch;
  String resultJson;
  String message;
  if (appTakeTaskResult(p.requestId, resultJson)) {
    JsonDocument r;
    deserializeJson(r, resultJson);
    const bool ok = String(r["result"]["status"] | "ok") == "ok";
    message = r["result"]["message"] | (ok ? "Done" : "Failed");
    Serial.println("[Switch] " + p.id + " reply: " + message);
    p.active = false;
    if (ok) return false;
  } else if ((int32_t)(millis() - p.deadline) >= 0) {
    Serial.println("[Switch] " + p.id + ": no reply within 5s — switched back");
    message = "No reply (timeout)";
    p.active = false;
  } else {
    return false;
  }
  setSwitchState(p.id.c_str(), p.previous);
  notice("switch-error", p.label + "\n" + message, LV_SYMBOL_WARNING,
         lv_palette_main(LV_PALETTE_AMBER));
  return true;
}

// --- Phone volume (local action "phoneVolume") ----------------------------------
//
// One control: a full-width slider, 0-100, with the value in large type
// above it. Drag it (or tap anywhere along it) and every phone volume step
// crossed sends one volume key press over BLE HID. A small back button sits
// top-left; BOOT also closes. The phone never reports its level, so the
// value is an estimate that starts mid-way and drifts if the phone's own
// buttons are used.

constexpr int kPhoneSteps = 16;   // iOS has 16 volume steps, Android media 15
constexpr uint32_t kVolumeIdleMs = 15000;
int gPhoneVolume = 50;            // 0-100, what the slider shows
int gPhoneStep = kPhoneSteps / 2; // the step last sent for it

int stepFor(int volume) { return (volume * kPhoneSteps + 50) / 100; }

struct VolumeUi {
  lv_obj_t *value;
};

void onVolumeSlider(lv_event_t *e) {
  auto *slider = static_cast<lv_obj_t *>(lv_event_get_target(e));
  auto *ui = static_cast<VolumeUi *>(lv_event_get_user_data(e));
  gPhoneVolume = lv_slider_get_value(slider);
  lv_label_set_text(ui->value, String(gPhoneVolume).c_str());
  const int step = stepFor(gPhoneVolume);
  if (step != gPhoneStep) {
    phoneHidVolumeSteps(step - gPhoneStep);
    Serial.printf("[Volume] %d (step %d -> %d)%s\n", gPhoneVolume, gPhoneStep, step,
                  phoneHidConnected() ? "" : " — phone not paired, not sent");
    gPhoneStep = step;
  }
}

void onVolumeBack(lv_event_t *) { inputInject(InputEvent::ButtonPressShort); }

void phoneVolumeScreen(const String &label) {
  lv_obj_t *scr = uiBlankScreen("phone-volume");

  // Small back button, top-left.
  lv_obj_t *back = lv_button_create(scr);
  lv_obj_set_size(back, 56, 40);
  lv_obj_set_pos(back, 6, 6);
  lv_obj_set_style_radius(back, 10, 0);
  lv_obj_set_style_bg_color(back, lv_color_hex(0x2A2A2A), 0);
  lv_obj_set_style_bg_color(back, lv_color_hex(0x4A4A4A), LV_STATE_PRESSED);
  lv_obj_set_style_shadow_width(back, 0, 0);
  lv_obj_set_ext_click_area(back, 8);
  lv_obj_add_event_cb(back, onVolumeBack, LV_EVENT_CLICKED, nullptr);
  lv_obj_t *backL = lv_label_create(back);
  lv_label_set_text(backL, LV_SYMBOL_LEFT);
  lv_obj_center(backL);

  lv_obj_t *title = lv_label_create(scr);
  lv_label_set_text(title, (String(LV_SYMBOL_VOLUME_MAX "  ") + label).c_str());
  lv_obj_set_style_text_color(title, lv_palette_main(LV_PALETTE_GREY), 0);
  lv_obj_align(title, LV_ALIGN_TOP_LEFT, 74, 16);

  static VolumeUi ui;
  ui.value = lv_label_create(scr);
  lv_obj_set_style_text_font(ui.value, &lv_font_montserrat_48, 0);
  lv_label_set_text(ui.value, String(gPhoneVolume).c_str());
  lv_obj_align(ui.value, LV_ALIGN_TOP_MID, 0, 46);

  lv_obj_t *slider = lv_slider_create(scr);
  lv_slider_set_range(slider, 0, 100);
  lv_slider_set_value(slider, gPhoneVolume, LV_ANIM_OFF);
  lv_obj_set_size(slider, 320 - 2 * 28, 14);
  lv_obj_align(slider, LV_ALIGN_TOP_MID, 0, 122);
  lv_obj_set_style_pad_all(slider, 10, LV_PART_KNOB);  // finger-sized knob
  // The slider owns every drag that starts on or near it: a 34px band above
  // and below the bar, and no gesture/scroll handling stealing the move.
  lv_obj_set_ext_click_area(slider, 34);
  lv_obj_remove_flag(slider, LV_OBJ_FLAG_GESTURE_BUBBLE);
  lv_obj_remove_flag(slider, LV_OBJ_FLAG_SCROLL_CHAIN);
  lv_obj_add_event_cb(slider, onVolumeSlider, LV_EVENT_VALUE_CHANGED, &ui);

  lv_obj_t *hint = lv_label_create(scr);
  lv_obj_set_style_text_color(hint, lv_palette_main(LV_PALETTE_GREY), 0);
  lv_obj_set_width(hint, 320 - 16);
  lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_long_mode(hint, LV_LABEL_LONG_DOT);
  lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -4);

  bool wasConnected = !phoneHidConnected();  // force the first refresh
  Serial.println("[Volume] drag the slider ('+'/'-' = encoder, 5 per turn); back or 'b' closes");
  uint32_t lastActivity = millis();
  while (true) {
    appPump();
    const bool connected = phoneHidConnected();
    if (connected != wasConnected) {
      wasConnected = connected;
      // The slider stays live either way; unpaired, moves just aren't sent.
      if (connected) {
        lv_obj_add_flag(hint, LV_OBJ_FLAG_HIDDEN);
      } else {
        lv_obj_remove_flag(hint, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(hint, (String("Pair '") + phoneHidName() + "' on your phone").c_str());
      }
    }
    const InputEvent ev = inputPoll();
    if (ev == InputEvent::EncoderCW || ev == InputEvent::EncoderCCW) {
      lv_slider_set_value(slider, gPhoneVolume + (ev == InputEvent::EncoderCW ? 5 : -5), LV_ANIM_OFF);
      lv_obj_send_event(slider, LV_EVENT_VALUE_CHANGED, nullptr);
      lastActivity = millis();
    } else if (ev != InputEvent::None) {
      return;  // back button / BOOT / push
    }
    if (millis() - lastActivity >= kVolumeIdleMs &&
        lv_display_get_inactive_time(nullptr) >= kVolumeIdleMs) {
      return;
    }
    delay(5);
  }
}

// --- Local actions (§5.2) -------------------------------------------------------

void runAction(JsonObjectConst node) {
  const String action = node["action"] | "";
  Serial.println("[Menu] action " + action);
  if (action == "phoneVolume") {
    phoneVolumeScreen(node["label"] | "Volume");
  } else if (action == "showAbout") {
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
// Touch: tap an item to open it, swipe left/right to page (pages slide),
// swipe down, swipe right on a folder's first page, or tap the "< Folder"
// header to go back. Encoder: turn moves a highlighted
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

bool isFolder(JsonObjectConst n) {
  return String(n["type"] | "") == "folder" && String(n["binding"] | "") != "message_inbox";
}

String connectionText() { return netMqttIsConnected() ? "Online" : "Offline"; }

struct Grid {
  lv_obj_t *status = nullptr;  // root only: connection label
};

Grid drawGrid(JsonArrayConst nodes, size_t page, int selected, const String &title, bool root,
              lv_screen_load_anim_t anim) {
  const size_t n = nodes.size();
  const size_t pages = (n + 1) / 2;
  Grid g;
  lv_obj_t *scr = uiBlankScreen(root ? "home" : "menu", anim);
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

    // Switches: icon lit amber when on, dimmed when off, a toggle under the
    // label; everything sits 4px higher to make room for it.
    const bool isSwitch = String(node["type"] | "") == "switch";
    const bool on = isSwitch && switchState(node);
    const int32_t top = isSwitch ? 8 : 12;
    const lv_color_t iconColor = !isSwitch ? lv_color_white()
                                 : on      ? lv_palette_main(LV_PALETTE_AMBER)
                                           : lv_palette_darken(LV_PALETTE_GREY, 1);

    // Config icon artwork (64px, §12) when it exists, else a 48px symbol in
    // the same 64px slot so labels line up across cells.
    const lv_image_dsc_t *art = iconImage(node["icon"] | "");
    lv_obj_t *icon;
    if (art) {
      icon = lv_image_create(cell);
      lv_image_set_src(icon, art);
      if (isSwitch) {
        lv_obj_set_style_image_recolor(icon, iconColor, 0);
        lv_obj_set_style_image_recolor_opa(icon, LV_OPA_COVER, 0);
      }
      lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, top);
    } else {
      icon = lv_label_create(cell);
      lv_label_set_text(icon, nodeSymbol(node));
      lv_obj_set_style_text_font(icon, &lv_font_montserrat_48, 0);
      lv_obj_set_style_text_color(icon, iconColor, 0);
      lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, top + 6);
    }

    if (isSwitch) {
      // Display only: taps land on the cell, which toggles it.
      lv_obj_t *toggle = lv_switch_create(cell);
      lv_obj_set_size(toggle, 52, 26);
      lv_obj_remove_flag(toggle, LV_OBJ_FLAG_CLICKABLE);
      lv_obj_add_flag(toggle, LV_OBJ_FLAG_EVENT_BUBBLE);
      lv_obj_set_style_bg_color(toggle, lv_palette_main(LV_PALETTE_AMBER),
                                LV_PART_INDICATOR | LV_STATE_CHECKED);
      if (on) lv_obj_add_state(toggle, LV_STATE_CHECKED);
      lv_obj_align(toggle, LV_ALIGN_BOTTOM_MID, 0, -6);
    }

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
    lv_obj_align(name, LV_ALIGN_TOP_MID, 0, top + 64 + (isSwitch ? 2 : 8));
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
  // How the next redraw arrives: entering a folder slides in from the right,
  // paging slides with the swipe, coming back from a folder slides from the left.
  lv_screen_load_anim_t anim = root ? LV_SCREEN_LOAD_ANIM_NONE : LV_SCREEN_LOAD_ANIM_MOVE_LEFT;
  const size_t pages = n ? (n + 1) / 2 : 1;
  while (true) {
    gTappedCell = -1;
    gGestured = false;
    Grid g = drawGrid(nodes, page, selected, title, root, anim);
    anim = LV_SCREEN_LOAD_ANIM_NONE;
    if (n == 0) uiShow("home-empty", "No menu items\nin the active config", LV_SYMBOL_LIST);
    bool wasOnline = netMqttIsConnected();

    InputEvent ev = InputEvent::None;
    while (ev == InputEvent::None) {
      appPump();
      ev = inputPoll();
      if (ev == InputEvent::None && pollSwitchReply()) break;  // undone: redraw
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
          if (!fwd && page == 0 && !root) {
            // Swiping right on a folder's first page backs out, like a
            // back gesture, instead of wrapping to its last page.
            Serial.println("[Menu] swipe -> back");
            return;
          }
          page = fwd ? (page + 1) % pages : (page + pages - 1) % pages;
          selected = -1;
          if (pages > 1) anim = fwd ? LV_SCREEN_LOAD_ANIM_MOVE_LEFT : LV_SCREEN_LOAD_ANIM_MOVE_RIGHT;
          Serial.printf("[Menu] swipe -> page %u/%u\n", page + 1, pages);
        } else {          // encoder: item by item
          const size_t before = page;
          selected = selected < 0 ? static_cast<int>(page * 2)
                                  : (fwd ? (selected + 1) % n : (selected + n - 1) % n);
          page = selected / 2;
          if (page != before) anim = fwd ? LV_SCREEN_LOAD_ANIM_MOVE_LEFT : LV_SCREEN_LOAD_ANIM_MOVE_RIGHT;
        }
        break;
      }
      case InputEvent::EncoderPressShort: {
        if (n == 0) break;
        const int idx = touched ? gTappedCell : (selected >= 0 ? selected : page * 2);
        if (!touched) selected = idx;
        openNode(nodes[idx], title);  // then redraw this level
        if (isFolder(nodes[idx])) anim = LV_SCREEN_LOAD_ANIM_MOVE_RIGHT;  // back from it
        break;
      }
      case InputEvent::ButtonPressShort:
      case InputEvent::ButtonPressLong:
        if (!root) return;
        if (page != 0) anim = LV_SCREEN_LOAD_ANIM_MOVE_RIGHT;
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
  } else if (type == "switch") {
    toggleSwitch(node);
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
