#pragma once

#include <Arduino.h>
#include <functional>

// Implements the firmware side of docs/PROVISIONING_PROTOCOL_V2.md —
// a session token from the on-screen QR, AES-256-GCM over a plain BLE GATT
// service. No protocomm, no protobuf, no SRP6a.
//
// KNOWN RISK: still the least-verified file in the POC. BLE, Wi-Fi and MQTT
// all run concurrently here, and the framing/counter logic is ours rather
// than a vetted library's. provisioningSelfTest() below checks the crypto
// against the spec's published vectors at boot; everything above that layer
// needs real hardware and a real client to exercise.

struct MqttProvisionedConfig {
  bool valid = false;
  String brokerHost;
  uint16_t brokerPort = 0;
  bool useTls = false;
  String clientId;
  String username;
  String password;
  String topicCommands;
  String topicCommandResults;
  String topicMessages;
  String topicConfig;
  String topicStatus;
};

// Runs the published test vectors from docs/PROVISIONING_PROTOCOL_V2.md §10
// against this firmware's own HKDF/AES-GCM/base64url code and logs a PASS or
// FAIL line per check. Cheap, and it turns "the app can't talk to the device"
// into an answerable question: if this passes, the firmware agrees with the
// spec and the bug is on the other side (or in the transport).
bool provisioningSelfTest();

struct WifiCredentials {
  bool valid = false;
  String ssid;
  String password;
};

// Wi-Fi credentials saved by the last successful provisioningRun().
//
// These are stored explicitly rather than read back from the Wi-Fi driver.
// WiFi.SSID()/WiFi.psk() report the *currently associated* AP, so on a fresh
// boot — before anything has associated — they are empty, and handing that to
// WiFi.begin() fails with "SSID too long or missing!" and then looks like a
// connection timeout.
WifiCredentials provisioningLoadStoredWifi();

bool provisioningIsProvisioned();

// Clears the provisioned flag (and stored MQTT config) so the next call to
// provisioningRun() re-enters BLE pairing. Does NOT clear Wi-Fi credentials
// stored by the Wi-Fi driver itself — see provisioning.cpp.
void provisioningForceReset();

// Blocks until Wi-Fi + MQTT config are both received and Wi-Fi is
// confirmed connected, or a hard failure/timeout occurs.
//
// onQrReady fires once, as soon as the session QR payload
// (PROVISIONING_PROTOCOL_V2.md §8) is available, so the caller can render it
// immediately rather than waiting for the whole flow to finish.
//
// onStatus fires on every phase change after the app has paired — first when
// the session authenticates, then around each Wi-Fi and MQTT attempt. The QR
// is spent by that point, so this is the caller's cue to replace it with
// progress UI. Both callbacks are invoked from this function's own task,
// never from a BLE callback, so they may safely touch LVGL.
// onTick is called continuously for the whole duration, including inside the
// multi-second Wi-Fi and MQTT attempts. Anything animated on screen needs it:
// this function owns the task for minutes at a time, so without it the caller
// gets no chance to redraw and a spinner sits frozen.
bool provisioningRun(const std::function<void(const String &qrPayload)> &onQrReady,
                      const std::function<void(const String &status)> &onStatus,
                      const std::function<void()> &onTick,
                      MqttProvisionedConfig &mqttConfigOut, uint32_t timeoutMs);

// For the already-provisioned path: loads the MQTT config saved by a
// previous successful provisioningRun().
MqttProvisionedConfig provisioningLoadStoredMqttConfig();
