#pragma once

#include <stdint.h>

// JD9853 panel init (LovyanGFX) + LVGL v9 wiring, plus the AXS5106L touch
// panel as an LVGL pointer input — Waveshare ESP32-C6-Touch-LCD-1.47 (see
// pins.h). Landscape, 320x172.

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

// Test aid: plays a finger press-drag-release through LVGL's touch input, as
// if the panel were touched (serial keys in main.cpp). Real touch is ignored
// while it runs.
void displaySimulateDrag(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint32_t ms);
