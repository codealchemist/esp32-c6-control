// Display bring-up smoke test — one-shot board-level sanity check, not part
// of the shipped app.
//
// Confirmed hardware (Waveshare ESP32-C6-LCD-1.47, ST7789 controller): pins,
// panel offsets, and the init register sequence below are taken directly
// from Waveshare's own example firmware
// (https://github.com/VolosR/waveshareBoards/tree/main/C6example,
// Display_ST7789.cpp/.h) rather than guessed, since that's known-working
// for this exact panel.
//
// What this does: brings the panel up and cycles the whole screen through a
// few solid colors, so you can visually confirm wiring + orientation are
// right before any real UI/LVGL code goes anywhere near it.

#include <Arduino.h>
#include <SPI.h>

#define PIN_MISO 5
#define PIN_MOSI 6
#define PIN_SCLK 7
#define PIN_CS 14
#define PIN_DC 15
#define PIN_RST 21
#define PIN_BL 22

#define SPI_FREQ 80000000

// Landscape orientation, matching Waveshare's default (LCD_WIDTH=320,
// LCD_HEIGHT=172 in their macros — physical panel is 172x320 portrait,
// driven rotated).
constexpr uint16_t LCD_W = 320;
constexpr uint16_t LCD_H = 172;
constexpr uint8_t MADCTL_LANDSCAPE = 0x60; // MY=0, MX=1, MV=1
constexpr int OFFSET_X = 0;
constexpr int OFFSET_Y = 34; // this panel's RAM is wider than the glass; centered via this offset

static void lcdCommand(uint8_t cmd) {
  SPI.beginTransaction(SPISettings(SPI_FREQ, MSBFIRST, SPI_MODE0));
  digitalWrite(PIN_CS, LOW);
  digitalWrite(PIN_DC, LOW);
  SPI.transfer(cmd);
  digitalWrite(PIN_CS, HIGH);
  SPI.endTransaction();
}

static void lcdData(uint8_t data) {
  SPI.beginTransaction(SPISettings(SPI_FREQ, MSBFIRST, SPI_MODE0));
  digitalWrite(PIN_CS, LOW);
  digitalWrite(PIN_DC, HIGH);
  SPI.transfer(data);
  digitalWrite(PIN_CS, HIGH);
  SPI.endTransaction();
}

static void lcdReset() {
  digitalWrite(PIN_CS, LOW);
  delay(50);
  digitalWrite(PIN_RST, LOW);
  delay(50);
  digitalWrite(PIN_RST, HIGH);
  delay(50);
}

static void lcdInit() {
  pinMode(PIN_CS, OUTPUT);
  pinMode(PIN_DC, OUTPUT);
  pinMode(PIN_RST, OUTPUT);
  SPI.begin(PIN_SCLK, PIN_MISO, PIN_MOSI);

  lcdReset();

  lcdCommand(0x11); // SLPOUT
  delay(120);

  lcdCommand(0x36); // MADCTL
  lcdData(MADCTL_LANDSCAPE);

  lcdCommand(0x3A); // COLMOD
  lcdData(0x05);    // 16bpp / RGB565

  lcdCommand(0xB0);
  lcdData(0x00);
  lcdData(0xE8);

  lcdCommand(0xB2);
  lcdData(0x0C);
  lcdData(0x0C);
  lcdData(0x00);
  lcdData(0x33);
  lcdData(0x33);

  lcdCommand(0xB7);
  lcdData(0x35);

  lcdCommand(0xBB);
  lcdData(0x35);

  lcdCommand(0xC0);
  lcdData(0x2C);

  lcdCommand(0xC2);
  lcdData(0x01);

  lcdCommand(0xC3);
  lcdData(0x13);

  lcdCommand(0xC4);
  lcdData(0x20);

  lcdCommand(0xC6);
  lcdData(0x0F);

  lcdCommand(0xD0);
  lcdData(0xA4);
  lcdData(0xA1);

  lcdCommand(0xD6);
  lcdData(0xA1);

  lcdCommand(0xE0);
  static const uint8_t gammaPos[] = {0xF0, 0x00, 0x04, 0x04, 0x04, 0x05, 0x29,
                                      0x33, 0x3E, 0x38, 0x12, 0x12, 0x28, 0x30};
  for (uint8_t b : gammaPos) lcdData(b);

  lcdCommand(0xE1);
  static const uint8_t gammaNeg[] = {0xF0, 0x07, 0x0A, 0x0D, 0x0B, 0x07, 0x28,
                                      0x33, 0x3E, 0x36, 0x14, 0x14, 0x29, 0x32};
  for (uint8_t b : gammaNeg) lcdData(b);

  lcdCommand(0x21); // INVON — this panel needs inversion on
  lcdCommand(0x11); // SLPOUT (again, matching the reference sequence)
  delay(120);
  lcdCommand(0x29); // DISPON
}

static void backlightInit() {
  ledcAttach(PIN_BL, 1000, 10);
  ledcWrite(PIN_BL, 1023); // full brightness
}

static void lcdFillScreen(uint16_t color565) {
  uint16_t xStart = 0 + OFFSET_X;
  uint16_t xEnd = LCD_W - 1 + OFFSET_X;
  uint16_t yStart = 0 + OFFSET_Y;
  uint16_t yEnd = LCD_H - 1 + OFFSET_Y;

  lcdCommand(0x2A); // CASET
  lcdData(xStart >> 8);
  lcdData(xStart & 0xFF);
  lcdData(xEnd >> 8);
  lcdData(xEnd & 0xFF);

  lcdCommand(0x2B); // RASET
  lcdData(yStart >> 8);
  lcdData(yStart & 0xFF);
  lcdData(yEnd >> 8);
  lcdData(yEnd & 0xFF);

  lcdCommand(0x2C); // RAMWR

  SPI.beginTransaction(SPISettings(SPI_FREQ, MSBFIRST, SPI_MODE0));
  digitalWrite(PIN_CS, LOW);
  digitalWrite(PIN_DC, HIGH);
  uint32_t pixelCount = (uint32_t)LCD_W * LCD_H;
  for (uint32_t i = 0; i < pixelCount; i++) {
    SPI.transfer16(color565);
  }
  digitalWrite(PIN_CS, HIGH);
  SPI.endTransaction();
}

struct NamedColor {
  const char *name;
  uint16_t rgb565;
};

static const NamedColor kColors[] = {
    {"RED", 0xF800}, {"GREEN", 0x07E0}, {"BLUE", 0x001F},
    {"WHITE", 0xFFFF}, {"BLACK", 0x0000},
};

void setup() {
  Serial.begin(115200);
  delay(2000);
  Serial.println();
  Serial.println("=== ST7789 display smoke test (Waveshare ESP32-C6-LCD-1.47) ===");

  lcdInit();
  backlightInit();

  Serial.println("Init done — cycling colors, check the screen.");
}

void loop() {
  for (const auto &c : kColors) {
    Serial.printf("Filling: %s\n", c.name);
    lcdFillScreen(c.rgb565);
    delay(1000);
  }
}
