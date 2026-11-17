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
//
// This module owns exactly ONE MQTT client. Calling this again stops and
// destroys any previous client first: two live clients sharing a clientId
// make the broker evict one, which auto-reconnects and evicts the other, in
// an endless takeover loop. On success the client is KEPT running so the
// device-topic functions below can use it; on failure it is destroyed.
MqttResult netConnectMqttAndTestEcho(const String &host, uint16_t port, bool useTls,
                                      const String &clientId, const String &username,
                                      const String &password, uint32_t timeoutMs,
                                      const std::function<void()> &onPoll = nullptr);

// --- Device application traffic (nodrige docs/00-poc-implementation-guide.md §0.6) ---

// Subscribes to `commandsTopic` on the live client (re-subscribed
// automatically after any reconnect) and records `messagesTopic` as the
// telemetry destination. Requires a prior successful
// netConnectMqttAndTestEcho().
bool netMqttStartDeviceTopics(const String &commandsTopic, const String &messagesTopic);

// Publishes {"componentId","eventType","value"} — the wire shape the Core's
// Connectivity Broker normalizes into a CoreEvent — to the messages topic.
bool netMqttPublishTelemetry(const char *componentId, const char *eventType, int value);

// Returns true (once) with the payload of the most recent message received
// on the commands topic. Commands arriving faster than this is polled
// collapse to the latest — fine for a POC display, not for a real inbox.
bool netMqttTakeCommand(String &payloadOut);

bool netMqttIsConnected();

// Stops and destroys the client, if any. Safe to call when none exists.
void netMqttStop();
