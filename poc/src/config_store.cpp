#include "config_store.h"

#include <ArduinoJson.h>
#include <LittleFS.h>

#include <set>
#include <vector>

#include "generated/icon_registry_gen.h"

namespace {

constexpr const char *kDefaultPath = "/config/default_config.json";
constexpr const char *kActivePath = "/config/active.json";
constexpr const char *kStagedPath = "/config/staged.json";

ConfigSummary gSummary;

// The loaded config, kept for the menu to render. Replaced only by
// configLoad(); a pushed config takes effect through a reboot (§5.3).
JsonDocument gActive;

bool readJson(const char *path, JsonDocument &doc) {
  File f = LittleFS.open(path, "r");
  if (!f) return false;
  const DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err) {
    Serial.printf("[Config] %s: JSON error: %s\n", path, err.c_str());
    return false;
  }
  return true;
}

// Write to staged.json, then rename over active.json (§5.3 point 3), so a
// power cut mid-write never leaves a truncated active config behind.
bool writeActive(const JsonDocument &doc) {
  File f = LittleFS.open(kStagedPath, "w");
  if (!f) return false;
  const size_t n = serializeJsonPretty(doc, f);
  f.close();
  if (n == 0) return false;
  LittleFS.remove(kActivePath);
  return LittleFS.rename(kStagedPath, kActivePath);
}

bool iconKnown(const char *id) {
  for (size_t i = 0; i < kIconRegistryCount; i++) {
    if (strcmp(kIconRegistry[i].id, id) == 0) return true;
  }
  // §5.3 point 5: also resolvable at runtime from /assets/icons/<id>.png.
  return LittleFS.exists(String("/assets/icons/") + id + ".png");
}

// --- Validation (§5.2, §5.2.1) -----------------------------------------------

struct Validator {
  ConfigSummary &s;
  bool printTree = true;
  std::set<String> ids;
  std::vector<String> firstErrors;  // reported back to the server on a push

  void error(const String &where, const String &msg) {
    s.errors++;
    if (firstErrors.size() < 5) firstErrors.push_back(where + ": " + msg);
    Serial.println("[Config] ERROR " + where + ": " + msg);
  }
  void warn(const String &where, const String &msg) {
    s.warnings++;
    Serial.println("[Config] warn  " + where + ": " + msg);
  }

  static bool isNum(JsonVariantConst v) { return v.is<float>() || v.is<int>(); }

  void checkParam(const String &where, JsonVariantConst p) {
    s.params++;
    const String key = p["key"] | "";
    const String type = p["type"] | "";
    const String at = where + ".params[" + key + "]";
    if (key.isEmpty()) error(at, "missing key");

    auto need = [&](const char *field) {
      if (p[field].isNull()) error(at, String("type '") + type + "' requires '" + field + "'");
    };

    if (type == "int" || type == "float") {
      need("min"); need("max"); need("step"); need("default");
      if (type == "float") need("decimals");
      if (isNum(p["min"]) && isNum(p["max"]) && isNum(p["default"])) {
        const float mn = p["min"], mx = p["max"], d = p["default"];
        if (mn > mx) error(at, "min > max");
        if (d < mn || d > mx) error(at, "default outside [min, max]");
      }
      if (isNum(p["step"]) && p["step"].as<float>() <= 0) error(at, "step must be > 0");
    } else if (type == "options") {
      need("default");
      JsonArrayConst choices = p["choices"];
      if (choices.isNull() || choices.size() == 0) {
        error(at, "options needs a non-empty 'choices' array");
      } else {
        bool defaultFound = false;
        for (JsonVariantConst c : choices) {
          if (c["value"].isNull() || c["label"].isNull()) error(at, "choice without value/label");
          if (c["value"] == p["default"]) defaultFound = true;
        }
        if (!defaultFound) error(at, "default is not one of the choices");
      }
    } else if (type == "bool") {
      need("default");
    } else if (type == "date" || type == "time") {
      need("format");
    } else if (type == "string") {
      need("maxLength"); need("charset");
    } else {
      error(at, "unknown param type '" + type + "'");
    }
  }

  void checkNode(JsonVariantConst n, const String &path, int depth) {
    const String id = n["id"] | "";
    const String type = n["type"] | "";
    const String label = n["label"] | "";
    const String where = path + "/" + (id.isEmpty() ? String("?") : id);

    if (id.isEmpty()) error(where, "missing id");
    else if (!ids.insert(id).second) error(where, "duplicate id");
    if (label.isEmpty()) error(where, "missing label");

    const char *icon = n["icon"] | static_cast<const char *>(nullptr);
    if (icon && !iconKnown(icon)) warn(where, String("icon '") + icon + "' has no artwork yet");

    // "shortcut": lets the shortcut button run this item. Remote actions only
    // (a button press has no way to pick a value): `true` on a switch (the
    // button toggles it) or a task (each press runs it), or on a task
    // { "off": {...} } — then presses alternate: run, then run with "off"
    // merged into the payload (e.g. a scene, then "Turn Off").
    JsonVariantConst sc = n["shortcut"];
    if (!sc.isNull()) {
      if (type != "task" && type != "switch") {
        error(where, "shortcut is only for task and switch items");
      } else if (sc.is<bool>()) {
        // fine
      } else if (type == "task" && sc.is<JsonObjectConst>()) {
        if (!sc["off"].isNull() && !sc["off"].is<JsonObjectConst>()) {
          error(where, "shortcut.off must be an object");
        }
      } else {
        error(where, type == "switch" ? "shortcut on a switch must be true or false"
                                      : "shortcut must be true, false or { \"off\": {...} }");
      }
    }

    String detail;
    if (type == "task") {
      s.tasks++;
      JsonVariantConst t = n["task"];
      if (!t["payload"].is<JsonObjectConst>()) error(where, "task.payload must be an object");
      const int qos = t["qos"] | 1;
      if (qos < 0 || qos > 2) error(where, "task.qos must be 0, 1 or 2");
      if (!t["expectsResult"].isNull() && !t["expectsResult"].is<bool>()) {
        error(where, "task.expectsResult must be a bool");
      }
      int nParams = 0;
      for (JsonVariantConst p : t["params"].as<JsonArrayConst>()) {
        checkParam(where, p);
        nParams++;
      }
      detail = "qos " + String(qos) + (t["expectsResult"] | false ? ", expects result" : "") +
               (nParams ? ", " + String(nParams) + " param(s)" : "");
    } else if (type == "switch") {
      // Two-state button (§5.2): each tap flips on/off and publishes it as a
      // task, with `payload` + the new state's own object (`on` / `off`).
      s.switches++;
      JsonVariantConst w = n["switch"];
      if (!w["payload"].is<JsonObjectConst>()) error(where, "switch.payload must be an object");
      for (const char *k : {"on", "off"}) {
        if (!w[k].isNull() && !w[k].is<JsonObjectConst>()) {
          error(where, String("switch.") + k + " must be an object");
        }
      }
      const int qos = w["qos"] | 1;
      if (qos < 0 || qos > 2) error(where, "switch.qos must be 0, 1 or 2");
      if (!w["expectsResult"].isNull() && !w["expectsResult"].is<bool>()) {
        error(where, "switch.expectsResult must be a bool");
      }
      if (!w["default"].isNull() && !w["default"].is<bool>()) {
        error(where, "switch.default must be a bool");
      }
      detail = "qos " + String(qos) + (w["expectsResult"] | false ? ", expects result" : "") +
               ", default " + (w["default"] | false ? "on" : "off");
    } else if (type == "folder") {
      s.folders++;
      const String binding = n["binding"] | "";
      if (!binding.isEmpty()) {
        if (binding != "message_inbox") error(where, "unknown binding '" + binding + "'");
        detail = "binding " + binding;
      } else if (!n["children"].is<JsonArrayConst>()) {
        error(where, "folder needs 'children' or a 'binding'");
      }
    } else if (type == "action") {
      s.actions++;
      const String action = n["action"] | "";
      if (action != "reprovision" && action != "restoreDefaultConfig" && action != "showAbout" &&
          action != "volume" && action != "phoneVolume" && action != "screenDim" &&
          action != "screenOff" && action != "switchSync" &&
          action != "shortcut") {
        error(where, "unknown action '" + action + "'");
      }
      detail = "action " + action;
    } else {
      error(where, "unknown node type '" + type + "'");
    }

    String indent;
    for (int i = 0; i < depth; i++) indent += "  ";
    if (printTree) Serial.printf("[Config] %s- [%s] %s \"%s\"%s\n", indent.c_str(), type.c_str(), id.c_str(),
                  label.c_str(), detail.isEmpty() ? "" : (" (" + detail + ")").c_str());

    if (type == "folder") {
      for (JsonVariantConst c : n["children"].as<JsonArrayConst>()) checkNode(c, where, depth + 1);
    }
  }

  // Structural problems that make the document unusable: these trigger the
  // fallback to the default. Node-level errors are reported but kept, so one
  // bad node is visible rather than silently replaced.
  bool checkRoot(JsonVariantConst root) {
    if (!root["configVersion"].is<int>() || root["configVersion"].as<int>() < 1) {
      error("root", "configVersion must be an integer >= 1");
      return false;
    }
    if (!root["menu"].is<JsonArrayConst>()) {
      error("root", "menu must be an array");
      return false;
    }
    if (!root["mqtt"].isNull() && !root["mqtt"].is<JsonObjectConst>()) {
      error("root", "mqtt must be an object");
      return false;
    }
    if (!root["mqtt"]["password"].isNull()) {
      warn("mqtt", "password must not be stored in the config (§5.1) — it is ignored");
    }
    return true;
  }
};

} // namespace

ConfigSummary configLoad() {
  gSummary = ConfigSummary{};
  JsonDocument doc;

  bool ok = false;
  if (readJson(kActivePath, doc)) {
    ConfigSummary probe;
    Validator v{probe};
    ok = v.checkRoot(doc);
    if (!ok) Serial.println("[Config] active.json is invalid — restoring the default");
  } else {
    Serial.println("[Config] no usable active.json — creating it from the default");
  }

  if (!ok) {
    doc.clear();
    if (!readJson(kDefaultPath, doc)) {
      Serial.println("[Config] default_config.json missing — did you run `pio run -t uploadfs`?");
      return gSummary;
    }
    ConfigSummary probe;
    Validator v{probe};
    if (!v.checkRoot(doc)) {
      Serial.println("[Config] default_config.json itself is invalid — not loading a config");
      return gSummary;
    }
    if (!writeActive(doc)) Serial.println("[Config] could not write active.json");
    gSummary.fromDefault = true;
  }

  gSummary.loaded = true;
  gActive = doc;
  gSummary.configVersion = doc["configVersion"];
  Serial.printf("[Config] configVersion %d%s — menu:\n", gSummary.configVersion,
                gSummary.fromDefault ? " (from default)" : "");
  Validator v{gSummary};
  for (JsonVariantConst n : doc["menu"].as<JsonArrayConst>()) v.checkNode(n, "", 1);
  Serial.printf("[Config] %d task(s), %d switch(es), %d folder(s), %d action(s), %d param(s); "
                "%d error(s), %d warning(s)\n",
                gSummary.tasks, gSummary.switches, gSummary.folders, gSummary.actions, gSummary.params,
                gSummary.errors, gSummary.warnings);
  return gSummary;
}

void configSyncMqtt(const MqttProvisionedConfig &m) {
  if (!m.valid || !gSummary.loaded) return;
  JsonDocument doc;
  if (!readJson(kActivePath, doc)) return;

  // as<>, not to<>: to<JsonObject>() empties an existing object, which made
  // every field look changed and rewrote active.json on every boot.
  JsonObject mq = doc["mqtt"].is<JsonObject>() ? doc["mqtt"].as<JsonObject>()
                                               : doc["mqtt"].to<JsonObject>();
  bool changed = false;
  auto set = [&](JsonObject o, const char *key, auto value) {
    if (o[key] != value) {
      o[key] = value;
      changed = true;
    }
  };
  set(mq, "brokerHost", m.brokerHost.c_str());
  set(mq, "brokerPort", m.brokerPort);
  set(mq, "useTls", m.useTls);
  set(mq, "clientId", m.clientId.c_str());
  set(mq, "username", m.username.c_str());
  JsonObject t = mq["topics"].is<JsonObject>() ? mq["topics"].as<JsonObject>()
                                               : mq["topics"].to<JsonObject>();
  set(t, "commands", m.topicCommands.c_str());
  set(t, "commandResults", m.topicCommandResults.c_str());
  set(t, "messages", m.topicMessages.c_str());
  set(t, "config", m.topicConfig.c_str());
  set(t, "status", m.topicStatus.c_str());
  if (mq["password"].is<const char *>()) {
    mq.remove("password");  // never persist it here (§5.1)
    changed = true;
  }

  if (!changed) return;
  if (writeActive(doc)) {
    Serial.println("[Config] active.json mqtt block updated from provisioning (" + m.clientId +
                   " @ " + m.brokerHost + ":" + String(m.brokerPort) + ")");
  } else {
    Serial.println("[Config] failed to update active.json mqtt block");
  }
}

String configSummaryLine() {
  if (!gSummary.loaded) return "Config: not loaded";
  String line = "Config: v" + String(gSummary.configVersion) + ", " +
                String(gSummary.tasks + gSummary.switches + gSummary.folders + gSummary.actions) + " nodes";
  if (gSummary.errors) line += ", " + String(gSummary.errors) + " errors";
  return line;
}

PushResult configApplyPushed(const String &json, String &resultJson) {
  JsonDocument result;
  result["type"] = "config";

  JsonDocument incoming;
  const DeserializationError err = deserializeJson(incoming, json);
  ConfigSummary probe;
  Validator v{probe, /*printTree=*/false};
  PushResult outcome = PushResult::Rejected;

  if (err) {
    result["errors"].add(String("invalid JSON: ") + err.c_str());
  } else if (v.checkRoot(incoming)) {
    for (JsonVariantConst n : incoming["menu"].as<JsonArrayConst>()) v.checkNode(n, "", 1);
    if (probe.errors == 0) outcome = PushResult::Applied;
  }
  result["configVersion"] = incoming["configVersion"] | 0;
  for (const String &e : v.firstErrors) result["errors"].add(e);

  if (outcome == PushResult::Applied) {
    JsonDocument active;
    const bool haveActive = readJson(kActivePath, active);

    // Unchanged? Compare only what a push may change. The broker keeps the
    // config retained and redelivers it on every (re)connect, so without
    // this the device would reboot on every connect.
    String inMenu, curMenu;
    serializeJson(incoming["menu"], inMenu);
    if (haveActive) serializeJson(active["menu"], curMenu);
    if (haveActive && inMenu == curMenu &&
        (incoming["configVersion"] | 0) == (active["configVersion"] | 0)) {
      outcome = PushResult::Unchanged;
    } else {
      // Connection settings come from provisioning, never from a push: a
      // pushed broker host that turns out wrong would cut the device off for
      // good, with no way to push the fix. Keep the local mqtt block.
      if (haveActive && active["mqtt"].is<JsonObject>()) {
        incoming["mqtt"] = active["mqtt"];
      } else {
        incoming.remove("mqtt");
      }
      if (!writeActive(incoming)) {
        outcome = PushResult::Rejected;
        result["errors"].add("could not write active.json");
      }
    }
  }

  result["status"] = outcome == PushResult::Applied     ? "applied"
                     : outcome == PushResult::Unchanged ? "unchanged"
                                                        : "rejected";
  Serial.printf("[Config] push v%d: %s (%d error(s))\n", result["configVersion"].as<int>(),
                result["status"].as<const char *>(), probe.errors);
  resultJson = "";
  serializeJson(result, resultJson);
  return outcome;
}

JsonArrayConst configMenu() { return gActive["menu"].as<JsonArrayConst>(); }

bool configRestoreDefault() {
  // Removing active.json is enough: the next configLoad() recreates it from
  // the default (§5.3 point 2), through the same validated path as first boot.
  const bool ok = !LittleFS.exists(kActivePath) || LittleFS.remove(kActivePath);
  Serial.println(ok ? "[Config] active.json removed — default restored on next boot"
                    : "[Config] could not remove active.json");
  return ok;
}
