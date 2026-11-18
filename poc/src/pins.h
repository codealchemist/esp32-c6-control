#pragma once

// Board: Waveshare ESP32-C6-Touch-LCD-1.47 — JD9853 LCD + AXS5106L capacitive
// touch. NOT the non-touch ESP32-C6-LCD-1.47 (ST7789, different pins) this
// POC originally targeted: on this board that config left the screen black.
//
// Every pin below is from Waveshare's own pin table and demo firmware for the
// touch board (docs.waveshare.com/ESP32-C6-Touch-LCD-1.47 and the
// 01_gfx_helloworld / esp_lcd_touch_axs5106l sources in its demo package).

// =============================================================================
// Display — JD9853 over 4-wire SPI. CONFIRMED.
// =============================================================================
#define PIN_LCD_SCLK 1
#define PIN_LCD_MOSI 2
#define PIN_LCD_MISO -1  // panel is write-only; GPIO3 is the TF card's MISO
#define PIN_LCD_CS 14
#define PIN_LCD_DC 15
#define PIN_LCD_RST 22
#define PIN_LCD_BL 23

// =============================================================================
// Touch — AXS5106L on I2C (bus shared with the onboard QMI8658 IMU). CONFIRMED.
// =============================================================================
#define PIN_TOUCH_SDA 18
#define PIN_TOUCH_SCL 19
#define PIN_TOUCH_RST 20
#define PIN_TOUCH_INT 21

// =============================================================================
// Physical inputs.
//
// PIN_BUTTON is the board's own BOOT button (GPIO8, active low, external
// pull-up) — real hardware, no wiring needed. Caveat: holding BOOT *while
// resetting* enters the ROM bootloader, so the "hold at boot to force
// re-provisioning" gesture only works if BOOT is pressed just after the
// reset, during the ~2s splash before inputInit() samples it.
//
// KY-040 rotary encoder, wired to the board's 22-pin header — see
// docs/WIRING_ROTARY_ENCODER.md. GPIO10/11 (the old placeholders) are not
// usable on this board: the ESP32-C6FH8's 32-pin package doesn't bring them
// out, and the header doesn't have them.
// The header's free pins are IO7, IO3/IO4 (TF card lines, unused here) and
// TXD/RXD (UART0, idle because the console is on USB):
//   - IO3 and IO4 already have 10K pull-ups to 3V3 on the board (R3-R6),
//     which suits the encoder's active-low outputs — but they are the TF
//     card's MISO/CS, so no TF card while the encoder is fitted.
//   - TXD is avoided: the ROM prints boot messages on it, driving the pin
//     while the encoder contact may be shorting it to GND.
// =============================================================================
#define PIN_BUTTON 8       // BOOT button — CONFIRMED
#define PIN_ENCODER_CLK 3  // KY-040 CLK -> header IO3
#define PIN_ENCODER_DT 7   // KY-040 DT  -> header IO7
#define PIN_ENCODER_SW 4   // KY-040 SW  -> header IO4
