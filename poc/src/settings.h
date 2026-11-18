#pragma once

#include <Arduino.h>

// On-device user settings (NVS, survive reboots and config pushes). These
// are the panel's own preferences, set from its Settings menu — not part of
// the pushed menu config.

constexpr uint32_t kDefaultDimAfterSec = 30;
constexpr uint32_t kDefaultOffAfterSec = 300;

// Seconds of inactivity before the backlight dims; 0 = never.
uint32_t settingsDimAfterSec();
void settingsSetDimAfterSec(uint32_t seconds);

// Seconds of inactivity before the screen turns off; 0 = never.
uint32_t settingsOffAfterSec();
void settingsSetOffAfterSec(uint32_t seconds);

// Volume page target.
constexpr uint8_t kVolumeTargetPhone = 0;
constexpr uint8_t kVolumeTargetComputer = 1;
uint8_t settingsVolumeTarget();
void settingsSetVolumeTarget(uint8_t target);
