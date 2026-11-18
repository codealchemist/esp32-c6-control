#include "menu.h"

#include <ArduinoJson.h>
#include <Preferences.h>
#include <WiFi.h>

#include <algorithm>
#include <vector>

#include <functional>

#include "app.h"
#include "config_store.h"
#include "input.h"
#include "generated/icon_registry_gen.h"
#include "display.h"
#include "net.h"
#include "phone_hid.h"
#include "settings.h"
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
    const String a = n["action"] | "";
    if (a == "volume" || a == "phoneVolume") return LV_SYMBOL_VOLUME_MAX;
    if (a == "screenDim") return LV_SYMBOL_EYE_CLOSE;
    if (a == "screenOff") return LV_SYMBOL_POWER;
    if (a == "switchSync") return LV_SYMBOL_REFRESH;
    return LV_SYMBOL_SETTINGS;
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


// --- In-tile task runs ------------------------------------------------------------
//
// Every task (a button that fires a remote action, §6.3) runs from its tile:
// no result screen. Adjustable params are asked first in their editor, then
// the grid comes back. A small spinner
// appears at the tile's top-right while the reply is due, then a green check
// or a red cross for a few seconds. Fire-and-forget tasks go straight to the
// check once sent. Details (the Core's message) go to the serial log.

constexpr uint32_t kBadgeShowMs = 3000;

struct TaskBadge {
  enum State { None, Busy, Ok, Error } state = None;
  String id, requestId;
  String scene;        // the task's tuyaScene (lowercase), if any
  uint32_t until = 0;  // Busy: reply deadline; Ok/Error: when to clear
};

void markSceneActive(const String &scene);
TaskBadge gBadges[4];

TaskBadge *badgeFor(const char *id) {
  for (TaskBadge &b : gBadges) {
    if (b.state != TaskBadge::None && b.id == id) return &b;
  }
  return nullptr;
}

TaskBadge &newBadge(const char *id) {
  if (TaskBadge *b = badgeFor(id)) return *b;
  for (TaskBadge &b : gBadges) {
    if (b.state == TaskBadge::None) return b;
  }
  for (TaskBadge &b : gBadges) {  // all taken: reuse one that isn't waiting
    if (b.state != TaskBadge::Busy) return b;
  }
  return gBadges[0];
}

void setBadge(TaskBadge &b, TaskBadge::State st, const char *why) {
  b.state = st;
  b.until = millis() + kBadgeShowMs;
  Serial.printf("[Task] %s: %s%s%s\n", b.id.c_str(), st == TaskBadge::Ok ? "done" : "FAILED",
                why && *why ? " - " : "", why ? why : "");
}


void startTileTask(JsonObjectConst node) {
  const char *id = node["id"] | "?";
  if (TaskBadge *b = badgeFor(id); b && b->state == TaskBadge::Busy) {
    Serial.printf("[Task] %s: still waiting for the last reply - ignored\n", id);
    return;
  }
  JsonObjectConst task = node["task"];
  // Adjustable params open their editor first (then back to the grid);
  // the rest travel at their default. Cancelling sends nothing.
  JsonDocument params;
  if (!collectParams(node, params.to<JsonObject>())) return;

  TaskBadge &b = newBadge(id);
  b = {};
  b.id = id;
  b.scene = task["payload"]["tuyaScene"] | "";
  b.scene.trim();
  b.scene.toLowerCase();
  // §6.3: no queue/replay — a stale "lights on" sent on reconnect would be
  // worse than saying it didn't go out.
  if (!netMqttIsConnected()) {
    setBadge(b, TaskBadge::Error, "offline, not sent");
    return;
  }
  String requestId;
  if (!publishTask(node, task, params.as<JsonObjectConst>(), requestId)) {
    setBadge(b, TaskBadge::Error, "not sent");
    return;
  }
  if (task["expectsResult"] | false) {
    b.state = TaskBadge::Busy;
    b.requestId = requestId;
    b.until = millis() + kResultTimeoutMs;
  } else {
    setBadge(b, TaskBadge::Ok, "sent");
  }
}

// Menu loop hook. True when a badge changed and the grid must be redrawn.
bool pollTileTasks() {
  bool changed = false;
  for (TaskBadge &b : gBadges) {
    if (b.state == TaskBadge::Busy) {
      String json;
      if (appTakeTaskResult(b.requestId, json)) {
        JsonDocument r;
        deserializeJson(r, json);
        const bool ok = String(r["result"]["status"] | "ok") == "ok";
        const String msg = r["result"]["message"] | "";
        setBadge(b, ok ? TaskBadge::Ok : TaskBadge::Error, msg.c_str());
        // The scene just ran: light its tile together with the check, not
        // when the Core's next scene_state arrives (it confirms or corrects
        // this a few seconds later, once it has read the lights back).
        if (ok && !b.scene.isEmpty()) markSceneActive(b.scene);
        changed = true;
      } else if ((int32_t)(millis() - b.until) >= 0) {
        setBadge(b, TaskBadge::Error, "no reply within 5s");
        changed = true;
      }
    } else if (b.state != TaskBadge::None && (int32_t)(millis() - b.until) >= 0) {
      b.state = TaskBadge::None;
      changed = true;
    }
  }
  return changed;
}

// The badge itself, at a tile's top-right corner.
void drawBadge(lv_obj_t *cell, const TaskBadge &b) {
  constexpr int32_t kSize = 26;
  if (b.state == TaskBadge::Busy) {
    lv_obj_t *sp = lv_spinner_create(cell);
    lv_spinner_set_anim_params(sp, 900, 240);
    lv_obj_set_size(sp, kSize, kSize);
    lv_obj_set_style_arc_width(sp, 4, LV_PART_MAIN);
    lv_obj_set_style_arc_width(sp, 4, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(sp, lv_color_hex(0x3A3A3A), LV_PART_MAIN);
    lv_obj_set_style_arc_color(sp, lv_palette_main(LV_PALETTE_BLUE), LV_PART_INDICATOR);
    lv_obj_remove_flag(sp, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(sp, LV_ALIGN_TOP_RIGHT, -6, 6);
    return;
  }
  const bool ok = b.state == TaskBadge::Ok;
  lv_obj_t *dot = lv_obj_create(cell);
  lv_obj_remove_style_all(dot);
  lv_obj_set_size(dot, kSize, kSize);
  lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(dot, lv_palette_main(ok ? LV_PALETTE_GREEN : LV_PALETTE_RED), 0);
  lv_obj_remove_flag(dot, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_align(dot, LV_ALIGN_TOP_RIGHT, -6, 6);
  lv_obj_t *mark = lv_label_create(dot);
  lv_label_set_text(mark, ok ? LV_SYMBOL_OK : LV_SYMBOL_CLOSE);
  lv_obj_set_style_text_font(mark, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(mark, lv_color_white(), 0);
  lv_obj_center(mark);
}

// --- Active scenes ---------------------------------------------------------------
//
// The Core tells the panel which Tuya scenes are active (scene_state command,
// lowercase scene names; see core-server-poc menu-tasks/tuya.js for how it
// knows). Tiles whose task payload names one of them as `tuyaScene` get a
// lit background. The panel asks only when a page with scene tiles opens;
// after a scene runs, its success reply lights the tile right away and
// nothing re-checks until the page is opened again.

std::vector<String> gActiveScenes;
bool gActiveScenesChanged = false;

void markSceneActive(const String &scene) {
  gActiveScenes.assign(1, scene);  // scenes set the same lights: one at a time
  gActiveScenesChanged = true;
}

bool sceneActive(JsonObjectConst node) {
  if (String(node["type"] | "") != "task") return false;
  String name = node["task"]["payload"]["tuyaScene"] | "";
  name.trim();
  name.toLowerCase();
  if (name.isEmpty()) return false;
  for (const String &a : gActiveScenes) {
    if (a == name) return true;
  }
  return false;
}

void requestSceneState() {
  JsonDocument node, spec;
  node["id"] = "scene-state";
  spec["payload"]["taskId"] = "scene-state";
  spec["qos"] = 0;
  String requestId;
  publishTask(node.as<JsonObjectConst>(), spec.as<JsonObjectConst>(), JsonObjectConst(), requestId);
}

bool pollSceneState() {
  if (!gActiveScenesChanged) return false;
  gActiveScenesChanged = false;
  return true;
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
    // Dimmed while offline; a tap just says so on the tile.
    TaskBadge &b = newBadge(id);
    b = {};
    b.id = id;
    setBadge(b, TaskBadge::Error, "offline, not switched");
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

// --- Switch state sync ----------------------------------------------------------------
//
// A switch's stored state can go stale (the lights were switched from the
// app or a wall switch). So the panel asks the Core for the real state —
// "switch-state", answered from the lights the switch's "on" scene acts on —
// once at boot (as soon as the server is reachable) and again on each screen
// wake, at most once per settingsSwitchSyncSec() (default 10 min; 0 = boot
// only). A reply that differs updates the tile.

constexpr uint32_t kSyncReplyMs = 10000;

struct SwitchSync {
  String requestId;
  JsonObjectConst node;
  uint32_t sentAt;
};
std::vector<SwitchSync> gSyncPending;
bool gSyncedOnce = false;
uint32_t gLastSync = 0;
uint32_t gSeenWake = 0;

void collectSwitches(JsonArrayConst nodes, std::vector<JsonObjectConst> &out) {
  for (JsonObjectConst n : nodes) {
    const String type = n["type"] | "";
    if (type == "switch") out.push_back(n);
    else if (type == "folder") collectSwitches(n["children"].as<JsonArrayConst>(), out);
  }
}

void syncSwitches(const char *why) {
  if (!netMqttIsConnected()) return;  // try again on the next trigger
  std::vector<JsonObjectConst> switches;
  collectSwitches(configMenu(), switches);
  int asked = 0;
  for (JsonObjectConst n : switches) {
    const char *onScene = n["switch"]["on"]["tuyaScene"] | "";
    if (!*onScene) continue;  // nothing the Core could read it from
    JsonDocument node, spec, extra;
    node["id"] = String(n["id"] | "?") + "-state";
    spec["payload"]["taskId"] = "switch-state";
    spec["qos"] = 1;
    spec["expectsResult"] = true;
    extra["onScene"] = onScene;
    String requestId;
    if (publishTask(node.as<JsonObjectConst>(), spec.as<JsonObjectConst>(),
                    extra.as<JsonObjectConst>(), requestId)) {
      gSyncPending.push_back({requestId, n, millis()});
      asked++;
    }
  }
  gSyncedOnce = true;
  gLastSync = millis();
  Serial.printf("[Switch] sync (%s): asked for %d switch state(s)\n", why, asked);
}

// Menu loop hook. True when a switch tile changed and the grid must redraw.
bool pollSwitchSync() {
  if (!gSyncedOnce) {
    syncSwitches("boot");
  } else if (displayWakeCount() != gSeenWake) {
    gSeenWake = displayWakeCount();
    const uint32_t every = settingsSwitchSyncSec();
    if (every > 0 && millis() - gLastSync >= every * 1000) syncSwitches("wake");
  }

  bool changed = false;
  for (size_t i = 0; i < gSyncPending.size();) {
    SwitchSync &p = gSyncPending[i];
    String json;
    const bool got = appTakeTaskResult(p.requestId, json);
    if (!got && millis() - p.sentAt < kSyncReplyMs) {
      i++;
      continue;
    }
    if (got) {
      JsonDocument r;
      deserializeJson(r, json);
      const char *id = p.node["id"] | "?";
      const String state = r["result"]["state"] | "";
      if (String(r["result"]["status"] | "") != "ok" || state.isEmpty()) {
        Serial.printf("[Switch] %s: state unknown (%s)\n", id, (r["result"]["message"] | "?"));
      } else if (gPendingSwitch.active && gPendingSwitch.id == id) {
        // The user just toggled it; their tap wins over this older reading.
      } else if ((state == "on") != switchState(p.node)) {
        setSwitchState(id, state == "on");
        Serial.printf("[Switch] %s: now %s (%s)\n", id, state.c_str(), (r["result"]["message"] | ""));
        changed = true;
      } else {
        Serial.printf("[Switch] %s: %s, unchanged\n", id, state.c_str());
      }
    }
    gSyncPending.erase(gSyncPending.begin() + i);
  }
  return changed;
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

void onPageBack(lv_event_t *) { inputInject(InputEvent::ButtonPressShort); }

// Header for single-control pages (Volume, Screen dimming): a small back
// button top-left (sends BOOT's "back") and a grey title beside it.
void makePageHeader(lv_obj_t *scr, const String &title) {
  lv_obj_t *back = lv_button_create(scr);
  lv_obj_set_size(back, 56, 40);
  lv_obj_set_pos(back, 6, 6);
  lv_obj_set_style_radius(back, 10, 0);
  lv_obj_set_style_bg_color(back, lv_color_hex(0x2A2A2A), 0);
  lv_obj_set_style_bg_color(back, lv_color_hex(0x4A4A4A), LV_STATE_PRESSED);
  lv_obj_set_style_shadow_width(back, 0, 0);
  lv_obj_set_ext_click_area(back, 8);
  lv_obj_add_event_cb(back, onPageBack, LV_EVENT_CLICKED, nullptr);
  lv_obj_t *backL = lv_label_create(back);
  lv_label_set_text(backL, LV_SYMBOL_LEFT);
  lv_obj_center(backL);

  lv_obj_t *t = lv_label_create(scr);
  lv_label_set_text(t, title.c_str());
  lv_obj_set_style_text_color(t, lv_palette_main(LV_PALETTE_GREY), 0);
  lv_obj_align(t, LV_ALIGN_TOP_LEFT, 74, 16);
}

// The page's one slider: full width, big value label above it, owns every
// drag that starts on or near it.
lv_obj_t *makePageSlider(lv_obj_t *scr, int32_t min, int32_t max, int32_t value) {
  lv_obj_t *slider = lv_slider_create(scr);
  lv_slider_set_range(slider, min, max);
  lv_slider_set_value(slider, value, LV_ANIM_OFF);
  lv_obj_set_size(slider, 320 - 2 * 28, 14);
  lv_obj_align(slider, LV_ALIGN_TOP_MID, 0, 122);
  lv_obj_set_style_pad_all(slider, 10, LV_PART_KNOB);  // finger-sized knob
  lv_obj_set_ext_click_area(slider, 34);
  lv_obj_remove_flag(slider, LV_OBJ_FLAG_GESTURE_BUBBLE);
  lv_obj_remove_flag(slider, LV_OBJ_FLAG_SCROLL_CHAIN);
  return slider;
}

lv_obj_t *makePageValue(lv_obj_t *scr, const String &text) {
  lv_obj_t *v = lv_label_create(scr);
  lv_obj_set_style_text_font(v, &lv_font_montserrat_48, 0);
  lv_label_set_text(v, text.c_str());
  lv_obj_align(v, LV_ALIGN_TOP_MID, 0, 46);
  return v;
}

// Volume page: one slider for either target, chosen with a small switch at
// the top-right (off = phone, on = computer; remembered in settings), with
// the current target's icon beside it.
//   Phone    — BLE HID volume keys; relative, so the level is an estimate.
//   Computer — the Core's "player-volume" task sets the computer's output
//              volume (0-100, absolute) and reports the real level back.
// While dragging for the computer, the latest level goes out at most every
// kComputerSendMs (fire-and-forget); on release a final set asks for the real
// level back.

constexpr uint32_t kComputerSendMs = 150;
int gComputerVolume = -1;  // -1 = not known yet

struct VolumePage {
  lv_obj_t *slider = nullptr, *value = nullptr, *hint = nullptr, *toggle = nullptr;
  lv_obj_t *targetIcon = nullptr;
  uint8_t target = 0;  // kVolumeTargetPhone / kVolumeTargetComputer
  bool computerDirty = false, released = false, hintDirty = true;
  uint32_t lastSent = 0;
  String pendingReply;  // requestId of a computer get/set awaiting its level
};
VolumePage gVol;

String volumeText(int v) { return v < 0 ? String("-") : String(v); }

// Sends player-volume for the computer: level < 0 = just read it.
void sendComputerVolume(int level, bool wantReply) {
  JsonDocument node, spec, extra;
  node["id"] = "computer-volume";
  spec["payload"]["taskId"] = "player-volume";
  spec["qos"] = wantReply ? 1 : 0;
  spec["expectsResult"] = wantReply;
  if (level >= 0) extra["level"] = level;
  String requestId;
  if (publishTask(node.as<JsonObjectConst>(), spec.as<JsonObjectConst>(), extra.as<JsonObjectConst>(),
                  requestId) && wantReply) {
    gVol.pendingReply = requestId;
  }
}

void showVolumeTarget() {
  const bool phone = gVol.target == kVolumeTargetPhone;
  if (phone) lv_obj_remove_state(gVol.toggle, LV_STATE_CHECKED);
  else lv_obj_add_state(gVol.toggle, LV_STATE_CHECKED);
  if (const lv_image_dsc_t *img = iconImage(phone ? "icon_phone" : "icon_computer")) {
    lv_image_set_src(gVol.targetIcon, img);
    lv_obj_align_to(gVol.targetIcon, gVol.toggle, LV_ALIGN_OUT_LEFT_MID, -8, 0);
  }
  const int v = phone ? gPhoneVolume : gComputerVolume;
  lv_label_set_text(gVol.value, volumeText(v).c_str());
  lv_slider_set_value(gVol.slider, v < 0 ? 50 : v, LV_ANIM_OFF);
  lv_obj_add_flag(gVol.hint, LV_OBJ_FLAG_HIDDEN);
}

void onVolumeToggle(lv_event_t *e) {
  auto *sw = static_cast<lv_obj_t *>(lv_event_get_target(e));
  const uint8_t t = lv_obj_has_state(sw, LV_STATE_CHECKED) ? kVolumeTargetComputer
                                                           : kVolumeTargetPhone;
  if (t == gVol.target) return;
  gVol.target = t;
  settingsSetVolumeTarget(t);
  gVol.hintDirty = true;
  Serial.printf("[Volume] target: %s\n", t == kVolumeTargetPhone ? "phone" : "computer");
  showVolumeTarget();
  if (t == kVolumeTargetComputer) sendComputerVolume(-1, true);  // fetch the real level
}

void onVolumeSlider(lv_event_t *e) {
  auto *slider = static_cast<lv_obj_t *>(lv_event_get_target(e));
  const int v = lv_slider_get_value(slider);
  lv_label_set_text(gVol.value, String(v).c_str());
  if (gVol.target == kVolumeTargetComputer) {
    gComputerVolume = v;
    gVol.computerDirty = true;
    return;
  }
  gPhoneVolume = v;
  const int step = stepFor(gPhoneVolume);
  if (step != gPhoneStep) {
    phoneHidVolumeSteps(step - gPhoneStep);
    Serial.printf("[Volume] phone %d (step %d -> %d)%s\n", gPhoneVolume, gPhoneStep, step,
                  phoneHidConnected() ? "" : " — phone not paired, not sent");
    gPhoneStep = step;
  }
}

void onVolumeReleased(lv_event_t *) { gVol.released = true; }

void showHint(const String &text) {
  lv_label_set_text(gVol.hint, text.c_str());
  lv_obj_remove_flag(gVol.hint, LV_OBJ_FLAG_HIDDEN);
}

void volumeScreen(const String &label) {
  lv_obj_t *scr = uiBlankScreen("volume");
  makePageHeader(scr, label);
  gVol = {};
  gVol.target = settingsVolumeTarget();

  // Target switch top-right: off = phone, on = computer, icon beside it.
  gVol.toggle = lv_switch_create(scr);
  lv_obj_set_size(gVol.toggle, 50, 26);
  lv_obj_align(gVol.toggle, LV_ALIGN_TOP_RIGHT, -10, 13);
  lv_obj_set_ext_click_area(gVol.toggle, 10);
  lv_obj_set_style_bg_color(gVol.toggle, lv_palette_main(LV_PALETTE_BLUE),
                            LV_PART_INDICATOR | LV_STATE_CHECKED);
  gVol.targetIcon = lv_image_create(scr);  // placed by showVolumeTarget()
  lv_obj_add_event_cb(gVol.toggle, onVolumeToggle, LV_EVENT_VALUE_CHANGED, nullptr);

  gVol.value = makePageValue(scr, "");
  gVol.slider = makePageSlider(scr, 0, 100, 50);
  lv_obj_add_event_cb(gVol.slider, onVolumeSlider, LV_EVENT_VALUE_CHANGED, nullptr);
  lv_obj_add_event_cb(gVol.slider, onVolumeReleased, LV_EVENT_RELEASED, nullptr);

  gVol.hint = lv_label_create(scr);
  lv_obj_set_style_text_color(gVol.hint, lv_palette_main(LV_PALETTE_GREY), 0);
  lv_obj_set_width(gVol.hint, 320 - 16);
  lv_obj_set_style_text_align(gVol.hint, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_long_mode(gVol.hint, LV_LABEL_LONG_DOT);
  lv_obj_align(gVol.hint, LV_ALIGN_BOTTOM_MID, 0, -4);

  showVolumeTarget();
  if (gVol.target == kVolumeTargetComputer) sendComputerVolume(-1, true);

  Serial.println("[Volume] drag the slider ('+'/'-' = encoder, 5 per turn); back or 'b' closes");
  uint32_t lastActivity = millis();
  bool wasPaired = phoneHidConnected();
  bool wasOnline = netMqttIsConnected();
  while (true) {
    appPump();
    const bool phone = gVol.target == kVolumeTargetPhone;

    // Status line: why moves may not land.
    const bool paired = phoneHidConnected(), online = netMqttIsConnected();
    if (paired != wasPaired || online != wasOnline || gVol.hintDirty) {
      wasPaired = paired;
      wasOnline = online;
      gVol.hintDirty = false;
      // Computer goes through the Core: offline, the switch can't move to it
      // (it can still move back to the phone).
      if (phone && !online) lv_obj_add_state(gVol.toggle, LV_STATE_DISABLED);
      else lv_obj_remove_state(gVol.toggle, LV_STATE_DISABLED);
      if (phone && !paired) showHint(String("Pair '") + phoneHidName() + "' on your phone");
      else if (!phone && !online) showHint("No server - computer volume unavailable");
      else lv_obj_add_flag(gVol.hint, LV_OBJ_FLAG_HIDDEN);
    }

    // Computer: confirm on release (one set that reports back); while still
    // dragging, stream the latest level.
    if (gVol.released) {
      gVol.released = false;
      if (!phone && gComputerVolume >= 0) {
        gVol.computerDirty = false;
        gVol.lastSent = millis();
        sendComputerVolume(gComputerVolume, true);
      }
    }
    if (!phone && gVol.computerDirty && millis() - gVol.lastSent >= kComputerSendMs) {
      gVol.computerDirty = false;
      gVol.lastSent = millis();
      sendComputerVolume(gComputerVolume, false);
    }
    String reply;
    if (!gVol.pendingReply.isEmpty() && appTakeTaskResult(gVol.pendingReply, reply)) {
      gVol.pendingReply = "";
      JsonDocument r;
      deserializeJson(r, reply);
      if (String(r["result"]["status"] | "") == "ok") {
        gComputerVolume = r["result"]["level"] | gComputerVolume;
        Serial.printf("[Volume] computer at %d%s\n", gComputerVolume,
                      (r["result"]["muted"] | false) ? " (muted)" : "");
        if (!phone && !lv_obj_has_state(gVol.slider, LV_STATE_PRESSED)) {
          lv_slider_set_value(gVol.slider, gComputerVolume, LV_ANIM_OFF);
          lv_label_set_text(gVol.value, volumeText(gComputerVolume).c_str());
          if (r["result"]["muted"] | false) showHint("Computer is muted");
          else lv_obj_add_flag(gVol.hint, LV_OBJ_FLAG_HIDDEN);
        }
      } else if (!phone) {
        showHint(r["result"]["message"] | "Computer volume failed");
      }
    }

    const InputEvent ev = inputPoll();
    if (ev == InputEvent::EncoderCW || ev == InputEvent::EncoderCCW) {
      const int cur = lv_slider_get_value(gVol.slider);
      lv_slider_set_value(gVol.slider, cur + (ev == InputEvent::EncoderCW ? 5 : -5), LV_ANIM_OFF);
      lv_obj_send_event(gVol.slider, LV_EVENT_VALUE_CHANGED, nullptr);
      gVol.released = true;  // a turn is a complete move
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

// --- Screen timeouts (local actions "screenDim", "screenOff") ----------------------
//
// How long without input before the backlight dims / the screen turns off: a
// slider over fixed choices, applied and saved as it moves. Back / BOOT
// closes. Any input wakes the screen (display.h), and that input does
// nothing else.

struct TimeoutSetting {
  const char *logName;
  const char *symbol;
  const char *hint;
  const uint32_t *choices;  // seconds; 0 = never
  int count;
  uint32_t (*get)();
  void (*set)(uint32_t);
  void (*apply)(uint32_t);
};

const uint32_t kDimChoices[] = {0, 10, 15, 30, 60, 120, 300, 600};
const uint32_t kOffChoices[] = {0, 30, 60, 120, 300, 600, 1800};

const TimeoutSetting kDimSetting = {
    "screen dim", LV_SYMBOL_EYE_CLOSE, "Dim the screen after this long idle", kDimChoices,
    sizeof(kDimChoices) / sizeof(kDimChoices[0]), settingsDimAfterSec, settingsSetDimAfterSec,
    displaySetAutoDim};
void applyNothing(uint32_t) {}  // read on the next screen wake

const uint32_t kSwitchSyncChoices[] = {0, 60, 300, 600, 1800, 3600};
const TimeoutSetting kSwitchSyncSetting = {
    "switch sync", LV_SYMBOL_REFRESH, "Recheck on wake; Never = boot only",
    kSwitchSyncChoices, sizeof(kSwitchSyncChoices) / sizeof(kSwitchSyncChoices[0]),
    settingsSwitchSyncSec, settingsSetSwitchSyncSec, applyNothing};

const TimeoutSetting kOffSetting = {
    "screen off", LV_SYMBOL_POWER, "Turn the screen off after this long idle", kOffChoices,
    sizeof(kOffChoices) / sizeof(kOffChoices[0]), settingsOffAfterSec, settingsSetOffAfterSec,
    displaySetAutoOff};

String timeoutText(uint32_t sec) {
  if (sec == 0) return "Never";
  if (sec < 60) return String(sec) + " s";
  return String(sec / 60) + " min";
}

int timeoutIndex(const TimeoutSetting &t, uint32_t sec) {
  int best = 0;
  for (int i = 0; i < t.count; i++) {
    if (t.choices[i] == sec) return i;
    if (t.choices[i] < sec) best = i;
  }
  return best;
}

struct TimeoutUi {
  const TimeoutSetting *setting;
  lv_obj_t *value;
};

void onTimeoutSlider(lv_event_t *e) {
  auto *slider = static_cast<lv_obj_t *>(lv_event_get_target(e));
  auto *ui = static_cast<TimeoutUi *>(lv_event_get_user_data(e));
  const TimeoutSetting &t = *ui->setting;
  const uint32_t sec = t.choices[lv_slider_get_value(slider)];
  if (sec == t.get()) return;
  t.set(sec);
  t.apply(sec);
  lv_label_set_text(ui->value, timeoutText(sec).c_str());
}

void timeoutScreen(const TimeoutSetting &t, const String &label) {
  lv_obj_t *scr = uiBlankScreen("timeout-setting");
  makePageHeader(scr, String(t.symbol) + "  " + label);
  const uint32_t current = t.get();
  static TimeoutUi ui;
  ui = {&t, makePageValue(scr, timeoutText(current))};
  lv_obj_t *slider = makePageSlider(scr, 0, t.count - 1, timeoutIndex(t, current));
  lv_obj_add_event_cb(slider, onTimeoutSlider, LV_EVENT_VALUE_CHANGED, &ui);

  lv_obj_t *hint = lv_label_create(scr);
  lv_label_set_text(hint, t.hint);
  lv_obj_set_style_text_color(hint, lv_palette_main(LV_PALETTE_GREY), 0);
  lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -4);

  Serial.printf("[Settings] %s: drag the slider ('+'/'-' = encoder); back or 'b' closes\n",
                t.logName);
  uint32_t lastActivity = millis();
  while (true) {
    appPump();
    const InputEvent ev = inputPoll();
    if (ev == InputEvent::EncoderCW || ev == InputEvent::EncoderCCW) {
      const int idx = lv_slider_get_value(slider) + (ev == InputEvent::EncoderCW ? 1 : -1);
      lv_slider_set_value(slider, idx, LV_ANIM_OFF);
      lv_obj_send_event(slider, LV_EVENT_VALUE_CHANGED, nullptr);
      lastActivity = millis();
    } else if (ev != InputEvent::None) {
      Serial.printf("[Settings] %s: %s\n", t.logName, timeoutText(t.get()).c_str());
      return;
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
  if (action == "volume" || action == "phoneVolume") {
    volumeScreen(node["label"] | "Volume");
  } else if (action == "screenDim") {
    timeoutScreen(kDimSetting, node["label"] | "Screen dimming");
  } else if (action == "screenOff") {
    timeoutScreen(kOffSetting, node["label"] | "Screen off");
  } else if (action == "switchSync") {
    timeoutScreen(kSwitchSyncSetting, node["label"] | "Switch sync");
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

// --- Device details (home header tap) ------------------------------------------------
//
// Full pages like the menu's: Connection (Wi-Fi, IP, signal, MQTT broker,
// Core version — asked from the Core when the page opens) and Device (chip,
// memory, uptime, battery, firmware build). Swipe left/right (or turn the
// encoder) to switch pages; back button, BOOT or swipe down closes; so does
// 30s without input. Values refresh every second.

constexpr uint32_t kDetailsIdleMs = 30000;
constexpr int kDetailsPages = 2;
constexpr int kDetailRows = 5;

void onGesture(lv_event_t *);  // the menu's swipe handler (Navigation, below)

// Battery: BAT_ADC on GPIO0 through a 200K/100K divider (VBAT = ADC x 3,
// Waveshare schematic). There is no battery-present signal: with no battery
// the charger (ETA6098) holds its battery pin near 4.2V, which reads exactly
// like a full one — so the top of the range is reported as "full or none".
String batteryText() {
  const uint32_t mv = analogReadMilliVolts(0) * 3;
  const String v = String(mv / 1000.0f, 2) + " V";
  if (mv < 2500) return "none";
  if (mv >= 4150) return v + " full/none";
  int pct = (int(mv) - 3300) * 100 / (4150 - 3300);
  pct = pct < 0 ? 0 : (pct > 100 ? 100 : pct);
  return v + "  " + String(pct) + "%";
}

String uptimeText() {
  const uint32_t s = millis() / 1000;
  const uint32_t d = s / 86400, h = s / 3600 % 24, m = s / 60 % 60;
  if (d) return String(d) + "d " + String(h) + "h " + String(m) + "m";
  if (h) return String(h) + "h " + String(m) + "m";
  return String(m) + "m " + String(s % 60) + "s";
}

// One page: header (back + title + page dots) and kDetailRows label/value rows.
struct DetailsPage {
  lv_obj_t *values[kDetailRows] = {};
};

DetailsPage drawDetailsPage(int page, lv_screen_load_anim_t anim) {
  static const char *kTitles[kDetailsPages] = {"Connection", "Device"};
  static const char *kLabels[kDetailsPages][kDetailRows] = {
      {"Wi-Fi", "IP", "Signal", "MQTT", "Core"},
      {"Chip", "Memory", "Uptime", "Battery", "Firmware"}};

  lv_obj_t *scr = uiBlankScreen("device-details", anim);
  lv_obj_add_event_cb(scr, onGesture, LV_EVENT_GESTURE, nullptr);
  makePageHeader(scr, kTitles[page]);

  for (int i = 0; i < kDetailsPages; i++) {  // page dots, top-right
    lv_obj_t *d = lv_obj_create(scr);
    lv_obj_remove_style_all(d);
    lv_obj_set_size(d, 8, 8);
    lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(d, i == page ? lv_color_white() : lv_palette_darken(LV_PALETTE_GREY, 2), 0);
    lv_obj_set_pos(d, 320 - 14 - (kDetailsPages - i) * 16 + 8, 22);
  }

  // Labels first, measured, so the value column starts past the widest one
  // ("Firmware") with a clear gap, whatever the font renders it at.
  lv_obj_t *labels[kDetailRows];
  int32_t labelW = 0;
  for (int i = 0; i < kDetailRows; i++) {
    labels[i] = lv_label_create(scr);
    lv_label_set_text(labels[i], kLabels[page][i]);
    lv_obj_set_style_text_color(labels[i], lv_palette_main(LV_PALETTE_GREY), 0);
    lv_obj_set_pos(labels[i], 12, 54 + i * 23);
    lv_obj_update_layout(labels[i]);
    labelW = std::max(labelW, lv_obj_get_width(labels[i]));
  }
  const int32_t valueX = 12 + labelW + 14;

  DetailsPage p;
  for (int i = 0; i < kDetailRows; i++) {
    lv_obj_t *v = lv_label_create(scr);
    lv_obj_set_width(v, 320 - valueX - 10);
    lv_label_set_long_mode(v, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(v, valueX, 54 + i * 23);
    p.values[i] = v;
  }
  return p;
}

void setValue(lv_obj_t *l, const String &text, lv_color_t color = lv_color_white()) {
  lv_label_set_text(l, text.c_str());
  lv_obj_set_style_text_color(l, color, 0);
}

void fillDetailsPage(int page, const DetailsPage &p, const String &coreText) {
  const lv_color_t white = lv_color_white(), bad = lv_palette_main(LV_PALETTE_RED);
  if (page == 0) {
    const bool wifi = WiFi.status() == WL_CONNECTED;
    const bool mqtt = netMqttIsConnected();
    setValue(p.values[0], wifi ? WiFi.SSID() : String("not connected"), wifi ? white : bad);
    setValue(p.values[1], wifi ? WiFi.localIP().toString() : String("-"));
    setValue(p.values[2], wifi ? String(WiFi.RSSI()) + " dBm" : String("-"));
    setValue(p.values[3], appBrokerHost().isEmpty() ? String("-") : appBrokerHost(),
             mqtt ? lv_palette_main(LV_PALETTE_GREEN) : bad);
    setValue(p.values[4], coreText, mqtt ? white : bad);
  } else {
    setValue(p.values[0], String(ESP.getChipModel()) + ", " + ESP.getCpuFreqMHz() + " MHz");
    setValue(p.values[1], String(ESP.getFlashChipSize() / (1024 * 1024)) + " MB, " +
                              String(ESP.getFreeHeap() / 1024) + " KB free");
    setValue(p.values[2], uptimeText());
    setValue(p.values[3], batteryText());
    setValue(p.values[4], __DATE__);
  }
}

void deviceDetailsScreen() {
  int page = 0;
  DetailsPage p = drawDetailsPage(page, LV_SCREEN_LOAD_ANIM_NONE);

  // Ask the Core who it is.
  String coreText = netMqttIsConnected() ? "asking..." : "offline";
  String coreRequest;
  const uint32_t coreAskedAt = millis();
  if (netMqttIsConnected()) {
    JsonDocument node, spec;
    node["id"] = "core-info";
    spec["payload"]["taskId"] = "core-info";
    spec["qos"] = 1;
    spec["expectsResult"] = true;
    publishTask(node.as<JsonObjectConst>(), spec.as<JsonObjectConst>(), JsonObjectConst(), coreRequest);
  }

  Serial.println("[Details] swipe or +/- for pages; back, swipe down or 'b' closes");
  uint32_t lastRefresh = 0;
  while (true) {
    appPump();

    String reply;
    if (!coreRequest.isEmpty() && appTakeTaskResult(coreRequest, reply)) {
      coreRequest = "";
      JsonDocument r;
      deserializeJson(r, reply);
      coreText = String("v") + (r["result"]["version"] | "?");
      lastRefresh = 0;
      Serial.printf("[Details] Core %s | battery %s | up %s\n", coreText.c_str(),
                    batteryText().c_str(), uptimeText().c_str());
    } else if (!coreRequest.isEmpty() && millis() - coreAskedAt > kResultTimeoutMs) {
      coreRequest = "";
      coreText = "no reply";
      lastRefresh = 0;
    }

    if (lastRefresh == 0 || millis() - lastRefresh >= 1000) {
      lastRefresh = millis();
      fillDetailsPage(page, p, coreText);
    }

    const InputEvent ev = inputPoll();
    if (ev == InputEvent::EncoderCW || ev == InputEvent::EncoderCCW) {
      const bool fwd = ev == InputEvent::EncoderCW;
      page = (page + (fwd ? 1 : kDetailsPages - 1)) % kDetailsPages;
      p = drawDetailsPage(page, fwd ? LV_SCREEN_LOAD_ANIM_MOVE_LEFT : LV_SCREEN_LOAD_ANIM_MOVE_RIGHT);
      lastRefresh = 0;
      Serial.printf("[Details] page %d/%d\n", page + 1, kDetailsPages);
    } else if (ev != InputEvent::None) {
      return;  // back button / BOOT / swipe down
    }
    if (lv_display_get_inactive_time(nullptr) >= kDetailsIdleMs) return;
    delay(5);
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

// What the home header shows: the server link needs Wi-Fi first, so a lost
// Wi-Fi reads as "No Wi-Fi" rather than "No server".
enum class Link { Online, NoServer, NoWifi };

Link linkState() {
  if (netMqttIsConnected()) return Link::Online;
  return netWifiConnected() ? Link::NoServer : Link::NoWifi;
}

void showLinkState(lv_obj_t *label, Link l) {
  const char *text = l == Link::Online   ? LV_SYMBOL_WIFI "  Online"
                     : l == Link::NoServer ? LV_SYMBOL_WIFI "  No server"
                                           : LV_SYMBOL_WARNING "  No Wi-Fi";
  const lv_palette_t c = l == Link::Online ? LV_PALETTE_GREEN
                         : l == Link::NoServer ? LV_PALETTE_AMBER
                                               : LV_PALETTE_RED;
  lv_label_set_text(label, text);
  lv_obj_set_style_text_color(label, lv_palette_main(c), 0);
}

// Tiles that need the Core (tasks, switches) are dimmed while it's unreachable.
bool needsCore(JsonObjectConst n) {
  const String t = n["type"] | "";
  return t == "task" || t == "switch";
}

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
    showLinkState(ht, linkState());
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
    if (sceneActive(node)) {  // the scene the lights are in right now
      lv_obj_set_style_bg_color(cell, lv_color_hex(0x5A4300), 0);
      lv_obj_set_style_bg_color(cell, lv_color_hex(0x7A5C00), LV_STATE_PRESSED);
    }
    if (static_cast<int>(idx) == selected) {
      lv_obj_set_style_border_width(cell, 2, 0);
      lv_obj_set_style_border_color(cell, lv_palette_main(LV_PALETTE_BLUE), 0);
    }
    lv_obj_add_event_cb(cell, onCellPressed, LV_EVENT_PRESSED, nullptr);
    lv_obj_add_event_cb(cell, onCellClicked, LV_EVENT_CLICKED,
                        reinterpret_cast<void *>(static_cast<intptr_t>(idx)));

    // Switches: icon lit amber when on, dimmed when off, and a toggle under
    // the label. To give the toggle clear space (12px above, 8px below) the
    // icon draws at 48px instead of 64 and the label keeps to one line:
    //   icon 8..56 | label 64..~94 | gap | toggle 106..130 | 8px
    const bool isSwitch = String(node["type"] | "") == "switch";
    const bool on = isSwitch && switchState(node);
    const bool dimmed = needsCore(node) && !netMqttIsConnected();
    const lv_color_t dimGrey = lv_palette_darken(LV_PALETTE_GREY, 2);
    const int32_t top = isSwitch ? 0 : 12;  // switch: 64px box, art drawn at 48 inside it
    const lv_color_t iconColor = dimmed    ? dimGrey
                                 : !isSwitch ? lv_color_white()
                                 : on        ? lv_palette_main(LV_PALETTE_AMBER)
                                             : lv_palette_darken(LV_PALETTE_GREY, 1);

    // Config icon artwork (64px, §12) when it exists, else a 48px symbol in
    // the same 64px slot so labels line up across cells.
    const lv_image_dsc_t *art = iconImage(node["icon"] | "");
    lv_obj_t *icon;
    if (art) {
      icon = lv_image_create(cell);
      lv_image_set_src(icon, art);
      if (isSwitch) lv_image_set_scale(icon, 192);  // 75%: 64 -> 48px, centred in its 64px box
      if (isSwitch || dimmed) {
        lv_obj_set_style_image_recolor(icon, iconColor, 0);
        lv_obj_set_style_image_recolor_opa(icon, LV_OPA_COVER, 0);
      }
      lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, top);
    } else {
      icon = lv_label_create(cell);
      lv_label_set_text(icon, nodeSymbol(node));
      lv_obj_set_style_text_font(icon, &lv_font_montserrat_48, 0);
      lv_obj_set_style_text_color(icon, iconColor, 0);
      lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, isSwitch ? 4 : top + 6);
    }

    if (isSwitch) {
      // Display only: taps land on the cell, which toggles it.
      lv_obj_t *toggle = lv_switch_create(cell);
      lv_obj_set_size(toggle, 48, 24);
      lv_obj_remove_flag(toggle, LV_OBJ_FLAG_CLICKABLE);
      lv_obj_add_flag(toggle, LV_OBJ_FLAG_EVENT_BUBBLE);
      lv_obj_set_style_bg_color(toggle, lv_palette_main(LV_PALETTE_AMBER),
                                LV_PART_INDICATOR | LV_STATE_CHECKED);
      if (on) lv_obj_add_state(toggle, LV_STATE_CHECKED);
      if (dimmed) lv_obj_add_state(toggle, LV_STATE_DISABLED);
      lv_obj_align(toggle, LV_ALIGN_BOTTOM_MID, 0, -8);
    }

    const String label = node["label"] | "?";
    lv_obj_t *name = lv_label_create(cell);
    lv_label_set_text(name, label.c_str());
    // Short labels at 28px; longer ones at 20px over up to two lines.
    const bool big = label.length() <= 9;
    lv_obj_set_style_text_font(name, big ? &lv_font_montserrat_28 : &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, 0);
    if (dimmed) lv_obj_set_style_text_color(name, dimGrey, 0);
    lv_obj_set_width(name, cellW - 12);
    if (!big) {
      lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
      lv_obj_set_height(name, (isSwitch ? 1 : 2) * lv_font_get_line_height(&lv_font_montserrat_20));
    }
    lv_obj_align(name, LV_ALIGN_TOP_MID, 0, isSwitch ? 64 : top + 64 + 8);

    if (const TaskBadge *b = badgeFor(node["id"] | "")) drawBadge(cell, *b);
  }
  return g;
}

// Shows one level until the user backs out. The root level never returns.
void showLevel(JsonArrayConst nodes, const String &title, bool root) {
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
  // Opening a page with scene tiles: ask the Core which scene is active.
  for (JsonObjectConst node : nodes) {
    if (!String(node["task"]["payload"]["tuyaScene"] | "").isEmpty()) {
      requestSceneState();
      break;
    }
  }
  while (true) {
    gTappedCell = -1;
    gGestured = false;
    Grid g = drawGrid(nodes, page, selected, title, root, anim);
    anim = LV_SCREEN_LOAD_ANIM_NONE;
    if (n == 0) uiShow("home-empty", "No menu items\nin the active config", LV_SYMBOL_LIST);
    const Link drawnLink = linkState();

    InputEvent ev = InputEvent::None;
    while (ev == InputEvent::None) {
      appPump();
      ev = inputPoll();
      if (ev == InputEvent::None) {
        const bool switchUndone = pollSwitchReply();
        const bool tasks = pollTileTasks();
        const bool synced = pollSwitchSync();
        if (pollSceneState() || tasks || switchUndone || synced) break;  // a tile changed: redraw
      }
      if (linkState() != drawnLink) {
        Serial.printf("[Menu] link: %s\n", linkState() == Link::Online     ? "online"
                                            : linkState() == Link::NoServer ? "no server"
                                                                            : "no Wi-Fi");
        break;  // redraw: header text, and remote tiles dim or light up
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
        if (root) deviceDetailsScreen();
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
    startTileTask(node);
  } else if (type == "switch") {
    toggleSwitch(node);
  } else if (type == "action") {
    runAction(node);
  } else if (type == "folder") {
    if (String(node["binding"] | "") == "message_inbox") {
      notice("inbox", label + "\nNo messages\n(inbox not built yet)", LV_SYMBOL_ENVELOPE,
             lv_color_white());
    } else {
      showLevel(node["children"].as<JsonArrayConst>(), label, false);
    }
  }
}

} // namespace

void menuSetActiveScenes(JsonArrayConst names) {
  gActiveScenes.clear();
  for (JsonVariantConst n : names) {
    String v = n | "";
    v.trim();
    v.toLowerCase();
    if (!v.isEmpty()) gActiveScenes.push_back(v);
  }
  gActiveScenesChanged = true;
  Serial.printf("[Scenes] active: %u\n", (unsigned)gActiveScenes.size());
}

void menuHome() {
  Serial.println("[Menu] home: tap an item to open, swipe left/right to page, swipe down or "
                 "tap the header to go back; encoder +/- selects, 's' opens, 'b' back, "
                 "'a' shows status");
  displaySetAutoDim(settingsDimAfterSec());
  displaySetAutoOff(settingsOffAfterSec());
  showLevel(configMenu(), "Menu", true);
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
