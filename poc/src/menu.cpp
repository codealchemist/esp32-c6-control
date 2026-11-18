#include "menu.h"

#include <ArduinoJson.h>

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
// One item per page: icon + label on top (tap = open), Back | Next below.
// Encoder: turn = next/previous, push = open. BOOT = back.

void showLevel(JsonArrayConst nodes, const String &path);

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
      showLevel(node["children"].as<JsonArrayConst>(), path + " / " + label);
    }
  }
}

void showLevel(JsonArrayConst nodes, const String &path) {
  const size_t n = nodes.size();
  if (n == 0) {
    notice("menu-empty", path + "\n(empty)", LV_SYMBOL_DIRECTORY, lv_color_white());
    return;
  }

  size_t index = 0;
  while (true) {
    JsonObjectConst node = nodes[index];
    UiView v = uiShow("menu", nullptr, nodeSymbol(node), lv_color_white(),
                      {{LV_SYMBOL_LEFT " Back", InputEvent::ButtonPressShort},
                       {"Next " LV_SYMBOL_RIGHT, InputEvent::EncoderCW}});
    uiSetContentAction(v, InputEvent::EncoderPressShort);

    lv_obj_t *where = lv_label_create(v.content);
    lv_label_set_text(where, (path + "  " + String(index + 1) + "/" + String(n)).c_str());
    lv_obj_set_style_text_color(where, lv_palette_main(LV_PALETTE_GREY), 0);
    lv_obj_set_width(where, LV_PCT(100));
    lv_label_set_long_mode(where, LV_LABEL_LONG_DOT);
    lv_obj_align(where, LV_ALIGN_TOP_LEFT, 0, 0);

    const String label = node["label"] | "?";
    lv_obj_t *name = lv_label_create(v.content);
    lv_label_set_text(name, label.c_str());
    // Short labels at 28px; long ones at 20 so they still fit on one line.
    lv_obj_set_style_text_font(name, label.length() <= 12 ? &lv_font_montserrat_28
                                                          : &lv_font_montserrat_20, 0);
    lv_obj_set_width(name, LV_PCT(100));
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    lv_obj_align(name, LV_ALIGN_LEFT_MID, 0, 0);

    const String type = node["type"] | "";
    lv_obj_t *hint = lv_label_create(v.content);
    lv_label_set_text(hint, type == "folder" ? "Tap to open" : type == "task" ? "Tap to send"
                                                                              : "Tap to run");
    lv_obj_set_style_text_color(hint, lv_palette_main(LV_PALETTE_GREY), 0);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    InputEvent ev = InputEvent::None;
    while (ev == InputEvent::None) {
      appPump();
      ev = inputPoll();
      delay(5);
    }
    if (ev == InputEvent::EncoderCW) {
      index = (index + 1) % n;
    } else if (ev == InputEvent::EncoderCCW) {
      index = (index + n - 1) % n;
    } else if (ev == InputEvent::EncoderPressShort) {
      openNode(node, path);  // then redraw this level
    } else if (ev == InputEvent::ButtonPressShort || ev == InputEvent::ButtonPressLong) {
      return;  // back
    }
  }
}

} // namespace

void menuRun() {
  Serial.println("[Menu] tap the item (or encoder push) to open; Next / turn to move; "
                 "Back / BOOT to go back");
  showLevel(configMenu(), "Menu");
  Serial.println("[Menu] closed");
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
