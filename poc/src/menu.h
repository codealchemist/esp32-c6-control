#pragma once

#include <Arduino.h>

#include <functional>

// The config-driven menu (docs/ARCHITECTURE.md §5.2, §6): renders the active
// config's node tree, runs tasks (publishing them to the Core) and local
// actions. Returns when the user backs out of the root level.
void menuRun();

// Generic int edit (§5.2.1 `int`): -/+ change a pending value shown on
// screen; tap the value area (or encoder push) sets it, BOOT or 5s idle
// cancels. Returns true and updates `value` only on set. `initialSteps`
// pre-applies an encoder turn that opened the editor; `footer`, if given,
// supplies the grey line under the value while nothing is pending.
bool uiEditInt(const String &title, int &value, int minValue, int maxValue, int step,
               int initialSteps = 0, std::function<String()> footer = nullptr);
