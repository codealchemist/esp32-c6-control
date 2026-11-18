#pragma once

#include <Arduino.h>

#include <ArduinoJson.h>

#include <functional>

// The config-driven menu (docs/ARCHITECTURE.md §5.2, §6): renders the active
// config's node tree, two items per page, runs tasks (publishing them to the
// Core) and local actions. This is the home screen while online: it never
// returns. Tapping its connection header opens the device details page.
void menuHome();

// Generic int edit (§5.2.1 `int`): -/+ change a pending value shown on
// screen; tap the value area (or encoder push) sets it, BOOT or 5s idle
// cancels. Returns true and updates `value` only on set. `initialSteps`
// pre-applies an encoder turn that opened the editor; `footer`, if given,
// supplies the grey line under the value while nothing is pending.
bool uiEditInt(const String &title, int &value, int minValue, int maxValue, int step,
               int initialSteps = 0, std::function<String()> footer = nullptr);

// The Core's scene_state command: which Tuya scenes (lowercase names) are
// active right now. Tiles running one of them are highlighted.
void menuSetActiveScenes(JsonArrayConst names);

// The Core's weather_alert push: the active alert list (header badge) and
// which are new (announced with a pop-up).
void menuWeatherAlerts(JsonVariantConst push);
