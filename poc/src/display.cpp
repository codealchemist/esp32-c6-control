#include "display.h"

#include <Arduino.h>
#include <LovyanGFX.hpp>
#include <lvgl.h>

#include "pins.h"

namespace {

class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_ST7789 _panel_instance;
  lgfx::Bus_SPI _bus_instance;
  lgfx::Light_PWM _light_instance;

public:
  LGFX() {
    {
      auto cfg = _bus_instance.config();
      cfg.spi_host = SPI2_HOST;
      cfg.spi_mode = 0;
      cfg.freq_write = 80000000;
      cfg.freq_read = 20000000;
      cfg.spi_3wire = false;
      cfg.use_lock = true;
      cfg.dma_channel = SPI_DMA_CH_AUTO;
      cfg.pin_sclk = PIN_LCD_SCLK;
      cfg.pin_mosi = PIN_LCD_MOSI;
      cfg.pin_miso = PIN_LCD_MISO;
      cfg.pin_dc = PIN_LCD_DC;
      _bus_instance.config(cfg);
      _panel_instance.setBus(&_bus_instance);
    }
    {
      auto cfg = _panel_instance.config();
      cfg.pin_cs = PIN_LCD_CS;
      cfg.pin_rst = PIN_LCD_RST;
      cfg.pin_busy = -1;
      // Native (rotation 0 / portrait) panel dimensions and offset — this
      // panel's controller RAM is wider than the glass, centered via this
      // offset, matching Waveshare's reference init (Offset_X=34 in
      // VERTICAL orientation there). setRotation(1) below is what gets us
      // to the 320x172 landscape orientation the rest of this project
      // assumes; LovyanGFX is expected to swap the offset for us on
      // rotation change — UNVERIFIED on real hardware, see display.h.
      cfg.panel_width = 172;
      cfg.panel_height = 320;
      cfg.offset_x = 34;
      cfg.offset_y = 0;
      cfg.offset_rotation = 0;
      cfg.readable = false;
      cfg.invert = true; // Waveshare's init sends 0x21 (INVON)
      cfg.rgb_order = false;
      cfg.dlen_16bit = false;
      cfg.bus_shared = false;
      _panel_instance.config(cfg);
    }
    {
      auto cfg = _light_instance.config();
      cfg.pin_bl = PIN_LCD_BL;
      cfg.invert = false;
      cfg.freq = 1000;
      cfg.pwm_channel = 0;
      _light_instance.config(cfg);
      _panel_instance.setLight(&_light_instance);
    }
    setPanel(&_panel_instance);
  }
};

LGFX gLcd;
lv_display_t *gDisplay = nullptr;

constexpr int kHorRes = 320;
constexpr int kVerRes = 172;
constexpr int kDrawBufLines = 40; // partial buffer, ~40 lines at a time

// LV_COLOR_DEPTH is expected to be 16 (RGB565) in lv_conf.h — see
// poc/README.md's lv_conf.h setup step.
uint8_t gDrawBuf[kHorRes * kDrawBufLines * 2];

void lvglFlushCb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map) {
  const uint32_t w = area->x2 - area->x1 + 1;
  const uint32_t h = area->y2 - area->y1 + 1;
  gLcd.startWrite();
  gLcd.setAddrWindow(area->x1, area->y1, w, h);
  gLcd.writePixels(reinterpret_cast<lgfx::rgb565_t *>(px_map), w * h);
  gLcd.endWrite();
  lv_display_flush_ready(disp);
}

uint32_t lvglTickCb() { return millis(); }

} // namespace

void displayInit() {
  gLcd.init();
  gLcd.setRotation(1); // landscape — see the offset/rotation note above
  gLcd.setBrightness(kBrightnessNormal);

  lv_init();
  lv_tick_set_cb(lvglTickCb);

  gDisplay = lv_display_create(kHorRes, kVerRes);
  lv_display_set_flush_cb(gDisplay, lvglFlushCb);
  lv_display_set_buffers(gDisplay, gDrawBuf, nullptr, sizeof(gDrawBuf),
                          LV_DISPLAY_RENDER_MODE_PARTIAL);
}

void displaySetBrightness(uint8_t level) { gLcd.setBrightness(level); }

void displayTick() { lv_timer_handler(); }
