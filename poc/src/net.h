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

struct DeviceTopics {
  String commands;        // subscribed: commands for this device
  String messages;        // published: task invocations (CoreEvents)
  String config;          // subscribed: config pushes (docs/ARCHITECTURE.md §5.3)
  String commandResults;  // published: results, incl. config apply outcomes
  String status;          // published, retained: {"state":"online",...}; last will "offline"
};

// Extra JSON fields for the retained "online" status message, without
// braces, e.g. "\"fw\":\"Nov 18 2026\",\"configVersion\":21". Set before
// netMqttBegin(); sent on every (re)connect.
void netMqttSetStatusInfo(const String &jsonFields);

// Subscribes to the commands and config topics on the live client
// (re-subscribed automatically after any reconnect) and records the publish
// topics. Requires a prior successful netConnectMqttAndTestEcho().
bool netMqttStartDeviceTopics(const DeviceTopics &topics);

// The panel's long-lived client: starts the one MQTT client with the device
// topics and returns at once. It connects, and reconnects after any drop,
// by itself (every 5s while the broker or Wi-Fi is away), re-subscribing
// each time; netMqttIsConnected() tells the current state. False only if the
// config is unusable.
bool netMqttBegin(const String &host, uint16_t port, bool useTls, const String &clientId,
                  const String &username, const String &password, const DeviceTopics &topics);

// Keeps Wi-Fi up in the background after boot: call netWifiKeepConnected()
// once with the stored credentials, then netWifiPoll() from the main loop
// (appPump does). Retries every 20s while disconnected.
void netWifiKeepConnected(const String &ssid, const String &password);
void netWifiPoll();
bool netWifiConnected();

// Pops the oldest message received on the commands topic (FIFO of 4; if it
// overflows, the oldest are dropped).
bool netMqttTakeCommand(String &payloadOut);

// Publishes a JSON event (CoreEvent wire shape) to the messages topic —
// task invocations travel this way (device -> Core).
bool netMqttPublishEvent(const String &json, int qos = 1);

// Returns true (once) with a complete config document received on the config
// topic. Messages larger than one MQTT buffer arrive in fragments and are
// reassembled first; documents over 8 KB are dropped (and logged).
bool netMqttTakeConfig(String &payloadOut);

// Publishes a JSON payload to the commandResults topic (QoS 1).
bool netMqttPublishResult(const String &json);

bool netMqttIsConnected();

// Stops and destroys the client, if any. Safe to call when none exists.
void netMqttStop();
