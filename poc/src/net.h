#pragma once

#include <Arduino.h>
#include <functional>

struct WifiResult {
  bool connected = false;
  String ip;
  String errorReason;
};

struct MqttResult {
  bool connected = false;
  bool echoOk = false; // subscribe+publish+receive round trip (docs/POC.md §3)
  String errorReason;
};

// `onPoll`, if given, is invoked repeatedly while these functions wait. Both
// block for many seconds, and nothing else on this task runs meanwhile — so
// without it any on-screen animation freezes for the whole attempt, which
// looks exactly like a crash. Callers that have no UI can omit it.
WifiResult netConnectWifi(const String &ssid, const String &password, uint32_t timeoutMs,
                           const std::function<void()> &onPoll = nullptr);

// Connects, then runs the pub/sub echo test on `<clientId>/poc-echo`.
// TLS verification is skipped when useTls is set (POC simplification —
// see net.cpp; the real app pins a CA cert per docs/ARCHITECTURE.md §8).
MqttResult netConnectMqttAndTestEcho(const String &host, uint16_t port, bool useTls,
                                      const String &clientId, const String &username,
                                      const String &password, uint32_t timeoutMs,
                                      const std::function<void()> &onPoll = nullptr);
