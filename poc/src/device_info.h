#pragma once

#include <vector>
#include <Arduino.h>

// Collects the device-info lines shown on the scrollable screen
// (docs/POC.md §2 step 3, §3's recommended free-heap/build-stamp fields).
std::vector<String> collectDeviceInfoLines();
