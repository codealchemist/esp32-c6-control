#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>

#include "provisioning.h"

// Config document — docs/ARCHITECTURE.md §5. POC scope: the §5.3 lifecycle
// (default -> active, runtime pushes over the config topic), validation
// against §5.2/§5.2.1, and filling the mqtt block from provisioning. Not yet
// here: rendering the menu (§6).
//
// Files on LittleFS:
//   /config/default_config.json  burned in via `pio run -t uploadfs`
//   /config/active.json          the live copy; recreated from the default
//                                whenever it is missing or invalid

struct ConfigSummary {
  bool loaded = false;
  bool fromDefault = false;  // active.json was (re)created from the default
  int configVersion = 0;
  int tasks = 0, switches = 0, folders = 0, actions = 0, params = 0;
  int errors = 0;
  int warnings = 0;  // e.g. icon ids with no baked-in artwork yet
};

// Loads /config/active.json, falling back to the default as above. Validates
// it, prints the menu tree and every problem found to Serial.
ConfigSummary configLoad();

// Writes the provisioned broker/identity/topics into active.json's mqtt block
// (§5.1's populated shape) if they differ from what is there. The MQTT
// password is deliberately NOT written — it stays in NVS only (§5.1).
void configSyncMqtt(const MqttProvisionedConfig &mqtt);

enum class PushResult { Applied, Unchanged, Rejected };

// Handles a config document pushed on the config topic (§5.3 point 3).
// Validates it with the same rules as configLoad(); if valid and different
// from the active config, writes it to active.json (via staged.json) — the
// caller then reboots to apply it. The mqtt block is always kept from the
// local active config, never taken from the push. Fills `resultJson` with
// {"type":"config","configVersion":N,"status":"applied|unchanged|rejected",
//  "errors":[...]} for the commandResults topic.
PushResult configApplyPushed(const String &json, String &resultJson);

// The active config's menu tree (§5.1), as loaded at boot. Empty if no config
// loaded.
JsonArrayConst configMenu();

// The "restoreDefaultConfig" action (§5.2): drop active.json so the next
// boot recreates it from default_config.json. Caller reboots.
bool configRestoreDefault();

// One-line summary for the Device Info screen.
String configSummaryLine();

// configVersion of the active config (0 before configLoad()).
int configActiveVersion();
