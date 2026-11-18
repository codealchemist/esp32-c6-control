#include "display.h"

#include <Arduino.h>
#include <LovyanGFX.hpp>
#include <lvgl.h>

#include "pins.h"
#include "touch.h"

namespace {

// JD9853 panel. Its command set is ST7789-compatible for everything LovyanGFX
// drives at runtime (CASET/RASET/RAMWR, MADCTL rotation, INVON, COLMOD), so it
// subclasses Panel_ST7789 and replaces only the power-on init list. The list
// is Waveshare's own lcd_reg_init() from the ESP32-C6-Touch-LCD-1.47 demo
// (01_gfx_helloworld), transcribed byte for byte. ST7789's generic init does
// NOT bring this controller up — that is what left the screen black.
struct Panel_JD9853 : public lgfx::Panel_ST7789 {
  const uint8_t *getInitCommands(uint8_t listno) const override {
    static constexpr uint8_t list0[] = {
        0x11, 0 + CMD_INIT_DELAY, 120,  // sleep out
        0xDF, 2, 0x98, 0x53,            // unlock vendor command set
        0xB2, 1, 0x23,
        0xB7, 4, 0x00, 0x47, 0x00, 0x6F,
        0xBB, 6, 0x1C, 0x1A, 0x55, 0x73, 0x63, 0xF0,
        0xC0, 2, 0x44, 0xA4,
        0xC1, 1, 0x16,
        0xC3, 8, 0x7D, 0x07, 0x14, 0x06, 0xCF, 0x71, 0x72, 0x77,
        0xC4, 12, 0x00, 0x00, 0xA0, 0x79, 0x0B, 0x0A, 0x16, 0x79, 0x0B, 0x0A, 0x16, 0x82,
        0xC8, 32, 0x3F, 0x32, 0x29, 0x29, 0x27, 0x2B, 0x27, 0x28, 0x28, 0x26, 0x25, 0x17,
                  0x12, 0x0D, 0x04, 0x00, 0x3F, 0x32, 0x29, 0x29, 0x27, 0x2B, 0x27, 0x28,
                  0x28, 0x26, 0x25, 0x17, 0x12, 0x0D, 0x04, 0x00,
        0xD0, 5, 0x04, 0x06, 0x6B, 0x0F, 0x00,
        0xD7, 2, 0x00, 0x30,
        0xE6, 1, 0x14,
        0xDE, 1, 0x01,
        0xB7, 5, 0x03, 0x13, 0xEF, 0x35, 0x35,
        0xC1, 3, 0x14, 0x15, 0xC0,
        0xC2, 2, 0x06, 0x3A,
        0xC4, 2, 0x72, 0x12,
        0xBE, 1, 0x00,
        0xDE, 1, 0x02,
        0xE5, 3, 0x00, 0x02, 0x00,
        0xE5, 3, 0x01, 0x02, 0x00,
        0xDE, 1, 0x00,
        0x35, 1, 0x00,                  // tearing effect line on
        0x3A, 1, 0x05,                  // RGB565
        0x2A, 4, 0x00, 0x22, 0x00, 0xCD,
        0x2B, 4, 0x00, 0x00, 0x01, 0x3F,
        0xDE, 1, 0x02,
        0xE5, 3, 0x00, 0x02, 0x00,
        0xDE, 1, 0x00,
        0x36, 1, 0x00,
        0x21, 0 + CMD_INIT_DELAY, 10,   // inversion on
        0x29, 0,                        // display on
        0xFF, 0xFF,
    };
    return listno == 0 ? list0 : nullptr;
  }
};

class LGFX : public lgfx::LGFX_Device {
  Panel_JD9853 _panel_instance;
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
      // Native (rotation 0 / portrait) panel dimensions and offset — the
      // controller RAM is wider than the glass, centred by this offset.
      // Waveshare's touch-board demo uses the same 34px column offset
      // (CASET 0x22..0xCD in the init list above). setRotation(1) below gives
      // the 320x172 landscape the rest of the project assumes; LovyanGFX
      // swaps the offset on rotation.
      cfg.panel_width = 172;
      cfg.panel_height = 320;
      cfg.offset_x = 34;
      cfg.offset_y = 0;
      cfg.offset_rotation = 0;
      cfg.readable = false;
      cfg.invert = true; // Waveshare's JD9853 init ends with 0x21 (INVON)
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

// A scripted finger for testing from the serial console (displaySimulateDrag):
// pressed from start to end, moving linearly, then released.
struct SimDrag {
  bool active = false;
  int16_t x0, y0, x1, y1;
  uint32_t start, ms;
} gSim;

bool simRead(int16_t &x, int16_t &y) {
  const uint32_t t = millis() - gSim.start;
  if (t >= gSim.ms) {
    gSim.active = false;  // this read reports the release, at the end point
    x = gSim.x1;
    y = gSim.y1;
    return false;
  }
  x = gSim.x0 + (int32_t)(gSim.x1 - gSim.x0) * (int32_t)t / (int32_t)gSim.ms;
  y = gSim.y0 + (int32_t)(gSim.y1 - gSim.y0) * (int32_t)t / (int32_t)gSim.ms;
  return true;
}

// Auto-dim (displaySetAutoDim): backlight drops after a stretch of no input;
// the next touch only wakes it — that whole press is hidden from LVGL so it
// can't also press whatever is under the finger.
uint32_t gAutoDimMs = 0;  // 0 = never
uint32_t gAutoOffMs = 0;  // 0 = never
enum class Screen { On, Dimmed, Off } gScreen = Screen::On;
bool gSwallowTouch = false;

void lvglTouchReadCb(lv_indev_t *, lv_indev_data_t *data) {
  int16_t x, y;
  bool pressed = gSim.active ? simRead(x, y) : touchRead(x, y);
  if (pressed && gScreen != Screen::On) {
    displayNoteActivity();  // wakes
    gSwallowTouch = true;
  }
  if (gSwallowTouch) {
    if (pressed) pressed = false;
    else gSwallowTouch = false;
  }
  data->point.x = x;
  data->point.y = y;
  data->state = pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

} // namespace

void displaySimulateDrag(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint32_t ms) {
  gSim = {true, x0, y0, x1, y1, millis(), ms ? ms : 1};
  Serial.printf("[Touch] simulated drag (%d,%d) -> (%d,%d) in %lums\n", x0, y0, x1, y1,
                (unsigned long)ms);
}

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

  // Touch as an LVGL pointer: buttons get clicks and lists scroll by drag
  // with no per-screen code. Registered even if the controller is absent —
  // touchRead() then always reports "released".
  touchInit();
  lv_indev_t *touch = lv_indev_create();
  lv_indev_set_type(touch, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(touch, lvglTouchReadCb);
}

void displaySetBrightness(uint8_t level) { gLcd.setBrightness(level); }

void displaySetAutoDim(uint32_t seconds) {
  gAutoDimMs = seconds * 1000;
  Serial.printf("[Display] dim %s\n", seconds ? (String("after ") + seconds + "s idle").c_str() : "never");
  displayNoteActivity();  // a changed timeout starts counting from now
}

void displaySetAutoOff(uint32_t seconds) {
  gAutoOffMs = seconds * 1000;
  Serial.printf("[Display] off %s\n", seconds ? (String("after ") + seconds + "s idle").c_str() : "never");
  displayNoteActivity();
}

bool displayNoteActivity() {
  lv_display_trigger_activity(gDisplay);
  if (gScreen == Screen::On) return false;
  if (gScreen == Screen::Off) {
    gLcd.wakeup();  // panel out of sleep (SLPOUT); its RAM kept the image
    lv_obj_invalidate(lv_screen_active());
  }
  gScreen = Screen::On;
  gLcd.setBrightness(kBrightnessNormal);
  Serial.println("[Display] wake");
  return true;
}

bool displayIsDimmed() { return gScreen != Screen::On; }

void displayTick() {
  // Idle ladder: on -> dimmed -> off. Either step can be "never"; if off
  // comes first the dim step is simply skipped.
  if (gScreen != Screen::Off && (gAutoDimMs || gAutoOffMs)) {
    const uint32_t idle = lv_display_get_inactive_time(gDisplay);
    if (gAutoOffMs && idle >= gAutoOffMs) {
      gScreen = Screen::Off;
      gLcd.setBrightness(0);
      gLcd.sleep();  // panel SLPIN: stops driving the LCD too, not just the backlight
      Serial.println("[Display] off (idle)");
    } else if (gScreen == Screen::On && gAutoDimMs && idle >= gAutoDimMs) {
      gScreen = Screen::Dimmed;
      gLcd.setBrightness(kBrightnessDim);
      Serial.println("[Display] dimmed (idle)");
    }
  }
  lv_timer_handler();
  // Every wait loop in the app pumps the display through here, so this is
  // where the loop watchdog is fed (enabled at the end of setup()). If the
  // app task ever stops coming back here for 5s — a hang in LVGL, a
  // deadlock — the task WDT panics and prints a backtrace instead of the
  // screen silently freezing.
  feedLoopWDT();
}
