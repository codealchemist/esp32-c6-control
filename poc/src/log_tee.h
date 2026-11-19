// Force-included into every firmware source (platformio.ini build_src_flags)
// so that `Serial` means LogTee: everything the firmware prints still goes
// to the USB serial console, and is also kept in a small ring buffer that
// the web console can stream over MQTT (diag.cpp). Reading from Serial
// (the serial key commands) passes straight through.
#pragma once
#ifdef __cplusplus
#include <Arduino.h>

class LogTee : public Stream {
public:
  void begin(unsigned long baud = 0);
  void setTxTimeoutMs(uint32_t ms);
  int available() override;
  int peek() override;
  int read() override;
  void flush() override;
  size_t write(uint8_t c) override;
  size_t write(const uint8_t *buf, size_t n) override;
  using Print::write;
  operator bool() const;

  // Bytes written since boot; the ring holds the last kRing of them.
  static constexpr size_t kRing = 6144;
  uint32_t written() const { return total_; }
  // Copies bytes [from, written()) that are still in the ring into out
  // (at most max) and returns how many; *from is moved past them. If the
  // ring already overwrote some, *from jumps ahead and *lost says how many.
  size_t readSince(uint32_t *from, char *out, size_t max, uint32_t *lost);

private:
  void keep(const uint8_t *buf, size_t n);
  char ring_[kRing];
  volatile uint32_t total_ = 0;
  portMUX_TYPE mux_ = portMUX_INITIALIZER_UNLOCKED;
};

extern LogTee gLogTee;

#undef Serial
#define Serial gLogTee
#endif
