#pragma once

// =============================================================================
// Display pins — CONFIRMED (Waveshare ESP32-C6-LCD-1.47, ST7789), from
// Waveshare's own example firmware. See docs/ARCHITECTURE.md §2.
// =============================================================================
#define PIN_LCD_MISO 5
#define PIN_LCD_MOSI 6
#define PIN_LCD_SCLK 7
#define PIN_LCD_CS 14
#define PIN_LCD_DC 15
#define PIN_LCD_RST 21
#define PIN_LCD_BL 22

// =============================================================================
// Input pins — **NOT CONFIRMED. EDIT BEFORE FLASHING.**
// These are placeholders only, chosen to avoid the display pins above and
// the C6's USB-JTAG pins (12/13), but NOT verified against your board's
// actual free GPIOs, silkscreen labels, or strapping-pin constraints
// (GPIO4/8/9 are strapping pins on the C6 — avoid them for anything that
// must read a stable level at boot, which button/encoder pins generally
// aren't, but worth knowing). See docs/POC.md §5.
// =============================================================================
#define PIN_BUTTON 2      // dedicated Back/Home button — CONFIRM
#define PIN_ENCODER_CLK 1 // KY-040 CLK — CONFIRM
#define PIN_ENCODER_DT 3  // KY-040 DT — CONFIRM
#define PIN_ENCODER_SW 10 // KY-040 push-button — CONFIRM
