#include "log_tee.h"

LogTee gLogTee;

void LogTee::begin(unsigned long baud) { HWCDCSerial.begin(baud); }
void LogTee::setTxTimeoutMs(uint32_t ms) { HWCDCSerial.setTxTimeoutMs(ms); }
int LogTee::available() { return HWCDCSerial.available(); }
int LogTee::peek() { return HWCDCSerial.peek(); }
int LogTee::read() { return HWCDCSerial.read(); }
void LogTee::flush() { HWCDCSerial.flush(); }
LogTee::operator bool() const { return (bool)HWCDCSerial; }

void LogTee::keep(const uint8_t *buf, size_t n) {
  // Any task may print (the MQTT event task does): a short critical
  // section around the copy, no allocation.
  portENTER_CRITICAL(&mux_);
  uint32_t t = total_;
  for (size_t i = 0; i < n; i++) ring_[(t + i) % kRing] = (char)buf[i];
  total_ = t + n;
  portEXIT_CRITICAL(&mux_);
}

size_t LogTee::write(uint8_t c) {
  keep(&c, 1);
  return HWCDCSerial.write(c);
}

size_t LogTee::write(const uint8_t *buf, size_t n) {
  keep(buf, n);
  return HWCDCSerial.write(buf, n);
}

size_t LogTee::readSince(uint32_t *from, char *out, size_t max, uint32_t *lost) {
  portENTER_CRITICAL(&mux_);
  const uint32_t t = total_;
  const uint32_t oldest = t > kRing ? t - kRing : 0;
  *lost = 0;
  if (*from < oldest) {
    *lost = oldest - *from;
    *from = oldest;
  }
  size_t n = t - *from;
  if (n > max) n = max;
  for (size_t i = 0; i < n; i++) out[i] = ring_[(*from + i) % kRing];
  *from += n;
  portEXIT_CRITICAL(&mux_);
  return n;
}
