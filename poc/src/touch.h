#pragma once

#include <stdint.h>

// AXS5106L capacitive touch controller (Waveshare ESP32-C6-Touch-LCD-1.47).
// Register protocol from Waveshare's esp_lcd_touch_axs5106l driver: I2C
// address 0x63, ID at 0x08, touch report (count + points) at 0x01.

// Resets the controller and checks it answers. Returns false if it does not
// (wrong board, or a bus problem) — the UI still works by buttons/serial.
bool touchInit();

// Current touch state in landscape screen coordinates (320x172, same
// orientation as the display). Cheap to call every frame: it only touches
// I2C after the controller signals new data, or while a finger is down.
bool touchRead(int16_t &x, int16_t &y);
