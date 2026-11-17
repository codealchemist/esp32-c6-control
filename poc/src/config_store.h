#pragma once

#include <Arduino.h>

#include "provisioning.h"

// Config document — docs/ARCHITECTURE.md §5. POC scope: the §5.3 lifecycle
// (default -> active), validation against §5.2/§5.2.1, and filling the mqtt
// block from provisioning. Not yet here: rendering the menu (§6) and runtime
// updates over the config topic (§5.3 point 3).
//
// Files on LittleFS:
//   /config/default_config.json  burned in via `pio run -t uploadfs`
//   /config/active.json          the live copy; recreated from the default
//                                whenever it is missing or invalid

struct ConfigSummary {
  bool loaded = false;
  bool fromDefault = false;  // active.json was (re)created from the default
  int configVersion = 0;
  int tasks = 0, folders = 0, actions = 0, params = 0;
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

// One-line summary for the Device Info screen.
String configSummaryLine();
