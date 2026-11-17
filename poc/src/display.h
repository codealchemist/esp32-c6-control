#pragma once

#include <stdint.h>

// ST7789 init (LovyanGFX) + LVGL v9 wiring, for the confirmed Waveshare
// ESP32-C6-LCD-1.47 pins/offsets (docs/ARCHITECTURE.md §2).
//
// KNOWN RISK: the panel offset/rotation values below (offset_x=34,
// setRotation(1)) are carried over from Waveshare's own raw-SPI reference
// init (see tools/display_smoke_test), translated into LovyanGFX's config
// struct rather than copied byte-for-byte — this translation has not been
// verified on real hardware. If the image appears shifted, cropped, or
// mirrored, this is the first place to check.

void displayInit();

// Normal viewing brightness, and the reduced level used while a QR code is
// on screen. A backlight at full blast blooms on this emissive panel: the
// white cells bleed into the dark ones and soften the module edges exactly
// when a camera is trying to focus on them. Dimming keeps the black/white
// contrast ratio identical while removing the glare.
constexpr uint8_t kBrightnessNormal = 255;
constexpr uint8_t kBrightnessQr = 128;

void displaySetBrightness(uint8_t level);

// Call every loop() iteration — pumps LVGL's timers/redraw and the
// LovyanGFX flush.
void displayTick();
