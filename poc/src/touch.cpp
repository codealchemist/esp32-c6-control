#include "touch.h"

#include <Arduino.h>
#include <Wire.h>

#include "pins.h"

namespace {

constexpr uint8_t kAddr = 0x63;
constexpr uint8_t kRegId = 0x08;
constexpr uint8_t kRegTouchData = 0x01;

// Native (portrait) panel size. Raw reports are in this frame.
constexpr int16_t kNativeW = 172;
constexpr int16_t kNativeH = 320;

// Read the controller at this interval regardless of the INT line. INT is
// still used to read sooner, but is not relied on: a missed or absent edge
// must not mean "never touched", and a stationary finger that stops
// generating edges must not look lifted (in LVGL a spurious release is a
// spurious click). 20ms ≈ LVGL's own input read period.
constexpr uint32_t kPollMs = 20;

// Logs every raw report that differs from the previous one. Toggled at
// runtime (serial 't', see touchSetDebug) so the touch path can be inspected
// on a deployed board without reflashing.
bool gTouchDebug = false;
uint8_t gLastRaw[6] = {0};

// Failed reads are otherwise silent — a bus fault would just look like
// "nobody is touching". Counted and reported periodically instead of per
// failure (at 50 reads/s, per-failure logging would flood the console).
uint32_t gReadErrors = 0;
uint32_t gReadOk = 0;
uint32_t gLastErrReportMs = 0;

volatile bool gIntFlag = false;
bool gReady = false;
bool gPressed = false;
int16_t gX = 0, gY = 0;
uint32_t gLastReadMs = 0;

void IRAM_ATTR onTouchInt() { gIntFlag = true; }

bool readReg(uint8_t reg, uint8_t *buf, size_t len) {
  // Register write and data read as two separate transactions (STOP in
  // between), exactly as Waveshare's ESP-IDF driver does. A repeated-start
  // read (endTransmission(false)) returned all-zero data from this part.
  Wire.beginTransmission(kAddr);
  Wire.write(reg);
  if (Wire.endTransmission(true) != 0) return false;
  if (Wire.requestFrom(kAddr, static_cast<uint8_t>(len)) != len) return false;
  for (size_t i = 0; i < len; i++) buf[i] = Wire.read();
  return true;
}

// Reads one report. Returns false on a bus error (state left unchanged).
bool poll() {
  uint8_t d[14];
  gLastReadMs = millis();
  if (!readReg(kRegTouchData, d, sizeof(d))) {
    gReadErrors++;
    if (millis() - gLastErrReportMs >= 5000) {
      gLastErrReportMs = millis();
      Serial.printf("[Touch] %u read error(s) so far (%u ok)\n", gReadErrors, gReadOk);
    }
    return false;
  }
  gReadOk++;

  if (gTouchDebug && memcmp(d, gLastRaw, sizeof(gLastRaw)) != 0) {
    memcpy(gLastRaw, d, sizeof(gLastRaw));
    Serial.printf("[Touch] raw %02x %02x %02x %02x %02x %02x\n", d[0], d[1], d[2], d[3], d[4],
                  d[5]);
  }

  // Low nibble is the point count; the high nibble carries flags (Waveshare's
  // ESP-IDF driver masks it the same way). Treating a flagged count as
  // "no touch" dropped every report.
  const uint8_t points = d[1] & 0x0F;
  if (points == 0 || points > 5) {
    if (gPressed) Serial.printf("[Touch] up   x=%d y=%d\n", gX, gY);
    gPressed = false;
    return true;
  }

  const int16_t rawX = ((d[2] & 0x0F) << 8) | d[3];
  const int16_t rawY = ((d[4] & 0x0F) << 8) | d[5];

  // Landscape mapping for display rotation 1, same as Waveshare's driver
  // for its rotation 1: screen x follows the panel's long axis (raw y),
  // screen y the short axis (raw x).
  const int16_t x = constrain(rawY, 0, kNativeH - 1);
  const int16_t y = constrain(rawX, 0, kNativeW - 1);

  if (!gPressed) {
    Serial.printf("[Touch] down x=%d y=%d (raw %d,%d)\n", x, y, rawX, rawY);
  }
  gPressed = true;
  gX = x;
  gY = y;
  return true;
}

} // namespace

bool touchInit() {
  Wire.begin(PIN_TOUCH_SDA, PIN_TOUCH_SCL, 400000);

  pinMode(PIN_TOUCH_RST, OUTPUT);
  digitalWrite(PIN_TOUCH_RST, LOW);
  delay(200);
  digitalWrite(PIN_TOUCH_RST, HIGH);
  delay(300);

  uint8_t id[3] = {0};
  if (!readReg(kRegId, id, sizeof(id))) {
    Serial.println("[Touch] AXS5106L not responding at 0x63 — touch disabled");
    return false;
  }
  Serial.printf("[Touch] AXS5106L ready (id %02x %02x %02x)\n", id[0], id[1], id[2]);

  pinMode(PIN_TOUCH_INT, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(PIN_TOUCH_INT), onTouchInt, FALLING);
  gReady = true;
  return true;
}

bool touchRead(int16_t &x, int16_t &y) {
  if (!gReady) return false;

  if (gIntFlag || millis() - gLastReadMs >= kPollMs) {
    gIntFlag = false;
    poll();
  }

  x = gX;
  y = gY;
  return gPressed;
}

void touchSetDebug(bool on) {
  gTouchDebug = on;
  memset(gLastRaw, 0xFF, sizeof(gLastRaw));  // print the next report even if unchanged
  Serial.printf("[Touch] debug %s (ready=%d, reads ok=%u, errors=%u, int=%d)\n", on ? "on" : "off",
                gReady, gReadOk, gReadErrors, digitalRead(PIN_TOUCH_INT));
}

bool touchDebugEnabled() { return gTouchDebug; }
