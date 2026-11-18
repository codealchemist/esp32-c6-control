#include "provisioning.h"

// Firmware side of docs/PROVISIONING_PROTOCOL_V2.md — token + AES-GCM over
// a plain BLE GATT service.
//
// Replaces the previous protocomm/network_provisioning (Security2/SRP6a)
// implementation entirely. Consequences of that swap worth knowing:
//
//  * Wi-Fi credentials now arrive on OUR endpoint and are applied by us.
//    v1 got them from the stock `prov-config` endpoint, which disappeared
//    along with protocomm — this is not a transport swap, that
//    functionality had to be rebuilt.
//  * No protobuf, no SRP6a, no Espressif client SDK on the app side.
//  * Session keys derive from the QR token, so there is NO forward secrecy
//    (PROVISIONING_PROTOCOL_V2.md §1). The token is single-use and never
//    persisted, which is what keeps that acceptable.
//
// UNVERIFIED ON HARDWARE. The BLE stack, Wi-Fi, and MQTT all run
// concurrently here for the first time.

#include <ArduinoJson.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <Preferences.h>
#include <WiFi.h>
#include <esp_mac.h>
#include <esp_random.h>
#include <string.h>

#include "net.h"
#include "prov_crypto.h"

namespace {

// --- Protocol constants (PROVISIONING_PROTOCOL_V2.md §3, §5, §7) ---------

constexpr char kServiceUuid[] = "8027151f-907e-49d2-bd90-234456b8a407";
constexpr char kRxCharUuid[] = "9e1fb9a1-0f07-45cd-ae0a-1d340cbd4a42";  // app -> device
constexpr char kTxCharUuid[] = "fbe43299-3649-4534-a091-b387f3b29aa1";  // device -> app

constexpr uint8_t kProtocolVersion = 2;

constexpr uint8_t kMsgHello = 0x01;
constexpr uint8_t kMsgHelloAck = 0x81;
constexpr uint8_t kMsgWifiCreds = 0x03;
constexpr uint8_t kMsgWifiStatus = 0x83;
constexpr uint8_t kMsgMqttConfig = 0x05;
constexpr uint8_t kMsgMqttStatus = 0x85;
constexpr uint8_t kMsgError = 0x8F;

constexpr char kHelloPlaintext[] = "PROVHELO";
constexpr size_t kHelloLen = 8;  // no NUL on the wire

constexpr size_t kHeaderLen = 10;   // ver(1) + type(1) + counter(8)
constexpr size_t kMaxFrameLen = 4096;

// Fragment payload per notification. Deliberately sized for the 23-byte BLE
// default MTU (23 - 3 ATT overhead - 1 fragment header) rather than a
// negotiated one: every device->app frame here is a short status JSON, so at
// most a handful of fragments, and this can never overrun a peer that
// declined to negotiate up.
constexpr size_t kNotifyChunk = 19;

constexpr const char *kPrefsNamespace = "poc_prov";
constexpr int kProvisioningProtocolVersion = 2;  // §7 mqtt-config payload

// --- Session state ------------------------------------------------------

struct Sec2Session {
  uint8_t kC2D[prov_crypto::kKeyLen] = {};
  uint8_t kD2C[prov_crypto::kKeyLen] = {};
  // Lowest counter still acceptable from the app. Starts at 0 (the app's
  // first frame), and every accepted frame pushes it past what was used —
  // which is exactly "reject counter <= highest accepted" (§5).
  uint64_t rxCounterMin = 0;
  uint64_t txCounter = 0;
  bool helloDone = false;
};

Sec2Session gSession;

// Set once in provisioningRun() before advertising starts, so the HELLO_ACK
// handler (which runs on the BLE task) can report the same name the QR and
// the advertisement carry.
String gServiceName;

BLECharacteristic *gTxChar = nullptr;
volatile bool gClientConnected = false;

// Set before BLE teardown begins. Stops onDisconnect() from restarting
// advertising on a server that is about to be destroyed — which is a
// use-after-free, not merely wasted work.
volatile bool gShuttingDown = false;

// Fragment reassembly (§6).
uint8_t gRxBuf[kMaxFrameLen];
size_t gRxLen = 0;
uint8_t gNextFragIdx = 0;

// Work handed from the BLE callback task to provisioningRun()'s loop.
//
// Single-core C6: the BLE host task and the app task never run
// simultaneously, and each flag is set only after its payload is fully
// written, so a reader that checks the flag first always sees complete data.
// This would need real synchronisation on a dual-core part.
volatile bool gWifiCredsPending = false;
String gPendingSsid;
String gPendingWifiPassword;

volatile bool gMqttConfigPending = false;
MqttProvisionedConfig gPendingMqttConfig;

volatile bool gWifiConnected = false;

// Set by the BLE task when HELLO authenticates; consumed by the run loop,
// which is where onStatus may actually be called from (LVGL is not
// thread-safe, so the callback must not run on the BLE host task).
volatile bool gHelloPending = false;

// --- Frame TX -----------------------------------------------------------

void sendFrame(uint8_t type, const uint8_t *plaintext, size_t ptLen) {
  // Gated on the connection alone, deliberately not on whether the peer has
  // subscribed to notifications. NimBLE services the CCCD internally, so a
  // descriptor write callback never fires here — gating sends on one would
  // silently turn every notify into a no-op and hang the session with
  // nothing logged. Notifying an unsubscribed peer is simply discarded by
  // the stack, which is by far the cheaper failure.
  if (gTxChar == nullptr || !gClientConnected) return;

  const uint64_t counter = gSession.txCounter++;

  uint8_t header[kHeaderLen];
  header[0] = kProtocolVersion;
  header[1] = type;
  for (int i = 0; i < 8; i++) {
    header[2 + i] = static_cast<uint8_t>((counter >> (56 - 8 * i)) & 0xFF);
  }

  // The nonce IS the counter (§5) — four zero bytes then the same 8 bytes
  // that are in the header. No random part, by design.
  uint8_t nonce[prov_crypto::kNonceLen] = {0, 0, 0, 0};
  memcpy(nonce + 4, header + 2, 8);

  const size_t frameLen = kHeaderLen + ptLen + prov_crypto::kTagLen;
  if (frameLen > kMaxFrameLen) return;

  uint8_t frame[kHeaderLen + 512 + prov_crypto::kTagLen];
  if (ptLen > 512) return;  // every device->app payload is a small JSON
  memcpy(frame, header, kHeaderLen);

  if (!prov_crypto::gcmSeal(gSession.kD2C, nonce, header, kHeaderLen, plaintext, ptLen,
                            frame + kHeaderLen, frame + kHeaderLen + ptLen)) {
    log_e("gcmSeal failed for type 0x%02X", type);
    return;
  }

  Serial.printf("[Prov] TX type 0x%02X counter %llu, %u byte payload, %u byte frame\n", type,
                counter, (unsigned)ptLen, (unsigned)frameLen);

  // Fragment (§6): one header byte, MORE in bit 7, index in bits 0-6.
  size_t offset = 0;
  uint8_t fragIdx = 0;
  while (offset < frameLen) {
    const size_t chunk = min(kNotifyChunk, frameLen - offset);
    const bool more = (offset + chunk) < frameLen;

    uint8_t packet[1 + kNotifyChunk];
    packet[0] = static_cast<uint8_t>((more ? 0x80 : 0x00) | (fragIdx & 0x7F));
    memcpy(packet + 1, frame + offset, chunk);

    gTxChar->setValue(packet, chunk + 1);
    gTxChar->notify();

    offset += chunk;
    fragIdx++;
    delay(5);  // pace notifications; NimBLE drops if the queue backs up
  }
}

void sendJson(uint8_t type, const JsonDocument &doc) {
  String out;
  serializeJson(doc, out);
  sendFrame(type, reinterpret_cast<const uint8_t *>(out.c_str()), out.length());
}

// --- Outbound queue -----------------------------------------------------
//
// Nothing may be transmitted from a BLE callback. handleFrame() and friends
// run on the NimBLE host task, and sendFrame() both calls notify() and
// delay()s between fragments — i.e. it blocks the very task responsible for
// pushing those notifications out. The symptom is brutal to diagnose: the
// device logs the request as handled and the client simply never receives
// the reply.
//
// Callbacks enqueue; provisioningRun()'s loop drains on the app task, where
// blocking is harmless.

struct TxItem {
  uint8_t type;
  String payload;
};

constexpr size_t kTxQueueLen = 4;
TxItem gTxQueue[kTxQueueLen];
volatile size_t gTxHead = 0;
volatile size_t gTxTail = 0;

void queueFrame(uint8_t type, const String &payload) {
  const size_t next = (gTxTail + 1) % kTxQueueLen;
  if (next == gTxHead) {
    Serial.printf("[Prov] TX queue full — dropped type 0x%02X\n", type);
    return;
  }
  gTxQueue[gTxTail].type = type;
  gTxQueue[gTxTail].payload = payload;
  gTxTail = next;  // published last, so a drain never sees a half-written slot
}

void queueJson(uint8_t type, const JsonDocument &doc) {
  String out;
  serializeJson(doc, out);
  queueFrame(type, out);
}

// App task only.
void drainTxQueue() {
  while (gTxHead != gTxTail) {
    TxItem &item = gTxQueue[gTxHead];
    sendFrame(item.type, reinterpret_cast<const uint8_t *>(item.payload.c_str()),
              item.payload.length());
    item.payload = String();  // release early; the queue outlives the send
    gTxHead = (gTxHead + 1) % kTxQueueLen;
  }
}

void queueError(const char *code, const char *message) {
  JsonDocument doc;
  doc["code"] = code;
  doc["message"] = message;
  queueJson(kMsgError, doc);
}

// --- Frame RX -----------------------------------------------------------

void handleWifiCreds(const uint8_t *json, size_t len) {
  JsonDocument doc;
  if (deserializeJson(doc, json, len)) {
    queueError("bad_json", "WIFI_CREDS payload is not valid JSON");
    return;
  }
  gPendingSsid = doc["ssid"].as<String>();
  gPendingWifiPassword = doc["password"] | "";
  gWifiCredsPending = true;  // set last — see the note on gWifiCredsPending
}

void handleMqttConfig(const uint8_t *json, size_t len) {
  // §9 step 8: mqtt-config is only meaningful once Wi-Fi is up, since the
  // device cannot act on it otherwise.
  if (!gWifiConnected) {
    queueError("wifi_not_connected", "send WIFI_CREDS and wait for connected first");
    return;
  }

  JsonDocument doc;
  if (deserializeJson(doc, json, len)) {
    queueError("bad_json", "MQTT_CONFIG payload is not valid JSON");
    return;
  }
  if (doc["provisioningProtocolVersion"].as<int>() != kProvisioningProtocolVersion) {
    JsonDocument err;
    err["code"] = "bad_version";
    err["message"] = "protocol version mismatch";
    err["deviceVersion"] = kProvisioningProtocolVersion;
    queueJson(kMsgError, err);
    return;
  }

  gPendingMqttConfig.brokerHost = doc["brokerHost"].as<String>();
  gPendingMqttConfig.brokerPort = doc["brokerPort"].as<uint16_t>();
  gPendingMqttConfig.useTls = doc["useTls"].as<bool>();
  gPendingMqttConfig.clientId = doc["clientId"].as<String>();
  gPendingMqttConfig.username = doc["username"].as<String>();
  gPendingMqttConfig.password = doc["password"] | "";
  gPendingMqttConfig.topicCommands = doc["topics"]["commands"].as<String>();
  gPendingMqttConfig.topicCommandResults = doc["topics"]["commandResults"].as<String>();
  gPendingMqttConfig.topicMessages = doc["topics"]["messages"].as<String>();
  gPendingMqttConfig.topicConfig = doc["topics"]["config"].as<String>();
  gPendingMqttConfig.topicStatus = doc["topics"]["status"].as<String>();
  gPendingMqttConfig.valid = true;
  gMqttConfigPending = true;  // set last
}

void handleFrame(const uint8_t *frame, size_t len) {
  if (len < kHeaderLen + prov_crypto::kTagLen) {
    Serial.printf("[Prov] frame too short: %u bytes, minimum %u\n", (unsigned)len,
                  (unsigned)(kHeaderLen + prov_crypto::kTagLen));
    return;
  }
  if (frame[0] != kProtocolVersion) {
    Serial.printf("[Prov] frame version 0x%02X, expected 0x%02X — dropped\n", frame[0],
                  kProtocolVersion);
    return;
  }

  const uint8_t type = frame[1];
  uint64_t counter = 0;
  for (int i = 0; i < 8; i++) {
    counter = (counter << 8) | frame[2 + i];
  }

  const size_t ctLen = len - kHeaderLen - prov_crypto::kTagLen;
  const uint8_t *ct = frame + kHeaderLen;
  const uint8_t *tag = frame + kHeaderLen + ctLen;

  uint8_t nonce[prov_crypto::kNonceLen] = {0, 0, 0, 0};
  memcpy(nonce + 4, frame + 2, 8);

  static uint8_t plaintext[kMaxFrameLen];
  // AAD is the header exactly as received, binding version/type/counter to
  // the ciphertext so none of them can be altered in flight.
  Serial.printf("[Prov] frame type 0x%02X counter %llu, %u byte payload\n", type, counter,
                (unsigned)ctLen);

  if (!prov_crypto::gcmOpen(gSession.kC2D, nonce, frame, kHeaderLen, ct, ctLen, tag, plaintext)) {
    // §9 step 4: stay silent to the client. Replying would confirm the
    // device to someone who cannot decrypt. Also: does NOT advance the
    // counter. Logged loudly here because on the wire this is invisible.
    Serial.printf(
        "[Prov] AUTH FAILED on type 0x%02X — dropped, no reply sent.\n"
        "       The tag did not verify, so either the client derived\n"
        "       different keys (check its HKDF salt = the 6 raw MAC bytes,\n"
        "       IKM = the 32 raw token bytes, and the exact info strings),\n"
        "       or its AAD is not the 10-byte header, or its nonce is not\n"
        "       4 zero bytes followed by the counter.\n",
        type);
    return;
  }

  // Replay rejection (§5). Only reached once the tag verified, so a forged
  // frame can never push the window forward.
  if (counter < gSession.rxCounterMin) {
    log_w("replayed/stale counter %llu (min %llu) — dropped", counter, gSession.rxCounterMin);
    return;
  }
  gSession.rxCounterMin = counter + 1;

  if (!gSession.helloDone && type != kMsgHello) {
    log_w("type 0x%02X before HELLO — dropped", type);
    return;
  }

  switch (type) {
    case kMsgHello: {
      if (ctLen != kHelloLen || memcmp(plaintext, kHelloPlaintext, kHelloLen) != 0) {
        log_w("HELLO payload mismatch — dropped");
        return;
      }
      gSession.helloDone = true;
      gHelloPending = true;  // run loop turns this into an onStatus() call
      Serial.println("[Prov] HELLO verified — session authenticated");

      JsonDocument ack;
      ack["ver"] = kProtocolVersion;
      ack["name"] = gServiceName;
      ack["fw"] = "poc-1";
      queueJson(kMsgHelloAck, ack);
      break;
    }
    case kMsgWifiCreds:
      handleWifiCreds(plaintext, ctLen);
      break;
    case kMsgMqttConfig:
      handleMqttConfig(plaintext, ctLen);
      break;
    default:
      log_w("unknown message type 0x%02X", type);
      break;
  }
}

void resetReassembly() {
  gRxLen = 0;
  gNextFragIdx = 0;
}

// Hex dump of the first few bytes, for the bring-up logging below.
String hexPreview(const uint8_t *data, size_t len, size_t maxBytes = 12) {
  String out;
  const size_t n = min(len, maxBytes);
  char buf[4];
  for (size_t i = 0; i < n; i++) {
    snprintf(buf, sizeof(buf), "%02x ", data[i]);
    out += buf;
  }
  if (len > n) out += "...";
  return out;
}

void handleFragment(const uint8_t *data, size_t len) {
  if (len < 1) return;

  // Deliberately verbose, and deliberately unconditional. Every "device
  // doesn't answer" failure — wrong keys, missing fragment header, a write
  // that never arrived — looks identical from the client, because §9 step 4
  // requires the device stay silent when a frame fails to authenticate.
  // This is the only place that distinguishes them.
  Serial.printf("[Prov] RX %u bytes: %s\n", (unsigned)len,
                hexPreview(data, len).c_str());

  const bool more = (data[0] & 0x80) != 0;
  const uint8_t idx = data[0] & 0x7F;

  if (idx != gNextFragIdx) {
    Serial.printf(
        "[Prov] fragment index %u, expected %u — discarded.\n"
        "       If the client sent a bare frame with no fragment header, the\n"
        "       first byte would be the version (0x02) and read as index 2.\n"
        "       Every write needs a leading header byte, 0x00 for a single\n"
        "       fragment (PROVISIONING_PROTOCOL_V2.md §6).\n",
        idx, gNextFragIdx);
    resetReassembly();
    return;
  }
  if (gRxLen + (len - 1) > kMaxFrameLen) {
    log_w("frame exceeds %u bytes — buffer discarded", kMaxFrameLen);
    resetReassembly();
    return;
  }

  memcpy(gRxBuf + gRxLen, data + 1, len - 1);
  gRxLen += len - 1;

  if (more) {
    gNextFragIdx++;
    return;
  }

  handleFrame(gRxBuf, gRxLen);
  resetReassembly();
}

// --- BLE plumbing -------------------------------------------------------

class RxCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *chr) override {
    String v = chr->getValue();
    handleFragment(reinterpret_cast<const uint8_t *>(v.c_str()), v.length());
  }
};

class ServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer *) override {
    gClientConnected = true;
    Serial.println("[Prov] BLE client connected");
  }
  void onDisconnect(BLEServer *server) override {
    gClientConnected = false;
    resetReassembly();
    if (gShuttingDown) {
      // Teardown is already underway on the app task. Re-advertising here
      // would touch a BLEServer in the middle of being freed.
      Serial.println("[Prov] BLE client disconnected (teardown in progress)");
      return;
    }
    Serial.println("[Prov] BLE client disconnected — re-advertising");
    server->startAdvertising();
  }
};

String deviceServiceName(const uint8_t mac[6]) {
  char buf[16];
  snprintf(buf, sizeof(buf), "PROV_%02X%02X%02X", mac[3], mac[4], mac[5]);
  return String(buf);
}

// --- Persistence --------------------------------------------------------

// Saved as soon as the credentials are proven to work, rather than at the end
// of the run: they are useful on the next boot even if MQTT later fails, and
// re-pairing just to recover a working SSID would be a poor trade.
void persistWifiCredentials(const String &ssid, const String &password) {
  Preferences prefs;
  prefs.begin(kPrefsNamespace, false);
  prefs.putString("wifiSsid", ssid);
  prefs.putString("wifiPass", password);
  prefs.end();
}

void persistMqttConfig(const MqttProvisionedConfig &cfg) {
  Preferences prefs;
  prefs.begin(kPrefsNamespace, false);
  prefs.putBool("provisioned", true);
  prefs.putString("mqttHost", cfg.brokerHost);
  prefs.putUShort("mqttPort", cfg.brokerPort);
  prefs.putBool("mqttTls", cfg.useTls);
  prefs.putString("mqttClientId", cfg.clientId);
  prefs.putString("mqttUser", cfg.username);
  prefs.putString("mqttPass", cfg.password);
  prefs.putString("tCmd", cfg.topicCommands);
  prefs.putString("tCmdRes", cfg.topicCommandResults);
  prefs.putString("tMsg", cfg.topicMessages);
  prefs.putString("tCfg", cfg.topicConfig);
  prefs.putString("tStat", cfg.topicStatus);
  prefs.end();
}

}  // namespace

namespace {

String toHex(const uint8_t *data, size_t len) {
  String out;
  out.reserve(len * 2);
  char buf[3];
  for (size_t i = 0; i < len; i++) {
    snprintf(buf, sizeof(buf), "%02x", data[i]);
    out += buf;
  }
  return out;
}

bool checkEq(const char *label, const String &got, const String &want) {
  const bool ok = (got == want);
  Serial.printf("[SelfTest] %-14s %s\n", label, ok ? "PASS" : "FAIL");
  if (!ok) {
    Serial.println("             got  " + got);
    Serial.println("             want " + want);
  }
  return ok;
}

}  // namespace

bool provisioningSelfTest() {
  // Fixed inputs from PROVISIONING_PROTOCOL_V2.md §10 — not a real session.
  uint8_t token[prov_crypto::kTokenLen];
  for (size_t i = 0; i < sizeof(token); i++) token[i] = static_cast<uint8_t>(i);
  const uint8_t mac[6] = {0xa1, 0xb2, 0xc3, 0xd4, 0xe5, 0xf6};

  bool ok = true;

  ok &= checkEq("base64url", prov_crypto::base64UrlEncode(token, sizeof(token)),
                "AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8");

  uint8_t kC2D[prov_crypto::kKeyLen], kD2C[prov_crypto::kKeyLen];
  prov_crypto::hkdfSha256(token, sizeof(token), mac, sizeof(mac), "esp32c6-prov-v2 c2d", kC2D,
                          sizeof(kC2D));
  prov_crypto::hkdfSha256(token, sizeof(token), mac, sizeof(mac), "esp32c6-prov-v2 d2c", kD2C,
                          sizeof(kD2C));
  ok &= checkEq("hkdf k_c2d", toHex(kC2D, sizeof(kC2D)),
                "e81929a65dc96fb77853751ecfcd741565022efd87007c225a069af8a677b43c");
  ok &= checkEq("hkdf k_d2c", toHex(kD2C, sizeof(kD2C)),
                "b11aad3d74d519382fba59ec4e809d5ca9073923f594f03887e76bdf7c2aa9a1");

  // HELLO, counter 0 — encrypt must reproduce the published frame byte for
  // byte, which pins nonce construction and AAD choice as well as the cipher.
  const uint8_t header[kHeaderLen] = {kProtocolVersion, kMsgHello, 0, 0, 0, 0, 0, 0, 0, 0};
  uint8_t nonce[prov_crypto::kNonceLen] = {0, 0, 0, 0};
  memcpy(nonce + 4, header + 2, 8);

  uint8_t sealed[kHeaderLen + kHelloLen + prov_crypto::kTagLen];
  memcpy(sealed, header, kHeaderLen);
  prov_crypto::gcmSeal(kC2D, nonce, header, kHeaderLen,
                       reinterpret_cast<const uint8_t *>(kHelloPlaintext), kHelloLen,
                       sealed + kHeaderLen, sealed + kHeaderLen + kHelloLen);
  ok &= checkEq("gcm seal", toHex(sealed, sizeof(sealed)),
                "020100000000000000005a818352d90cebc73c20e49dbffe4cd7d290b17fcd2fc7b9");

  // ...and decrypt back, which is the path a real HELLO takes.
  uint8_t opened[kHelloLen + 1] = {};
  const bool opened_ok =
      prov_crypto::gcmOpen(kC2D, nonce, header, kHeaderLen, sealed + kHeaderLen, kHelloLen,
                           sealed + kHeaderLen + kHelloLen, opened);
  ok &= checkEq("gcm open", opened_ok ? String(reinterpret_cast<char *>(opened)) : "<auth failed>",
                "PROVHELO");

  // A tampered tag MUST fail. If this passes, authentication is not actually
  // being checked and every other result above is meaningless.
  uint8_t tampered[sizeof(sealed)];
  memcpy(tampered, sealed, sizeof(sealed));
  tampered[sizeof(tampered) - 1] ^= 0x01;
  const bool rejected =
      !prov_crypto::gcmOpen(kC2D, nonce, header, kHeaderLen, tampered + kHeaderLen, kHelloLen,
                            tampered + kHeaderLen + kHelloLen, opened);
  ok &= checkEq("gcm reject", rejected ? "rejected" : "ACCEPTED", "rejected");

  Serial.printf("[SelfTest] === crypto %s ===\n", ok ? "OK" : "FAILED");
  return ok;
}

WifiCredentials provisioningLoadStoredWifi() {
  WifiCredentials creds;
  Preferences prefs;
  prefs.begin(kPrefsNamespace, true);
  creds.ssid = prefs.getString("wifiSsid", "");
  creds.password = prefs.getString("wifiPass", "");
  prefs.end();
  creds.valid = creds.ssid.length() > 0;
  return creds;
}

bool provisioningIsProvisioned() {
  Preferences prefs;
  prefs.begin(kPrefsNamespace, true);
  const bool flag = prefs.getBool("provisioned", false);
  const String ssid = prefs.getString("wifiSsid", "");
  prefs.end();

  // Both halves required, not just the flag. A device provisioned by a build
  // that predates persistWifiCredentials() has the flag set but no SSID
  // stored — and treating that as "provisioned" strands it permanently: every
  // boot skips the QR, then fails Wi-Fi for want of credentials it can no
  // longer obtain. Demanding both makes such a device fall back to pairing on
  // its own instead of needing a manual NVS wipe.
  if (flag && ssid.length() == 0) {
    Serial.println("[Prov] provisioned flag set but no stored SSID — re-pairing");
    return false;
  }
  return flag;
}

void provisioningForceReset() {
  Preferences prefs;
  prefs.begin(kPrefsNamespace, false);
  prefs.clear();
  prefs.end();
}

MqttProvisionedConfig provisioningLoadStoredMqttConfig() {
  MqttProvisionedConfig cfg;
  Preferences prefs;
  prefs.begin(kPrefsNamespace, true);
  cfg.valid = prefs.getBool("provisioned", false);
  cfg.brokerHost = prefs.getString("mqttHost", "");
  cfg.brokerPort = prefs.getUShort("mqttPort", 0);
  cfg.useTls = prefs.getBool("mqttTls", false);
  cfg.clientId = prefs.getString("mqttClientId", "");
  cfg.username = prefs.getString("mqttUser", "");
  cfg.password = prefs.getString("mqttPass", "");
  cfg.topicCommands = prefs.getString("tCmd", "");
  cfg.topicCommandResults = prefs.getString("tCmdRes", "");
  cfg.topicMessages = prefs.getString("tMsg", "");
  cfg.topicConfig = prefs.getString("tCfg", "");
  cfg.topicStatus = prefs.getString("tStat", "");
  prefs.end();
  return cfg;
}

bool provisioningRun(const std::function<void(const String &qrPayload)> &onQrReady,
                      const std::function<void(const String &status)> &onStatus,
                      const std::function<void()> &onTick,
                      MqttProvisionedConfig &mqttConfigOut, uint32_t timeoutMs) {
  // Local helper so every call site is a one-liner and the null check lives
  // in exactly one place.
  auto report = [&onStatus](const String &text) {
    if (onStatus) onStatus(text);
  };

  gSession = Sec2Session{};
  gWifiCredsPending = false;
  gMqttConfigPending = false;
  gWifiConnected = false;
  gShuttingDown = false;
  gClientConnected = false;
  gHelloPending = false;
  // Discard anything queued by a previous session.
  gTxHead = 0;
  gTxTail = 0;
  gPendingMqttConfig = MqttProvisionedConfig{};
  resetReassembly();

  // esp_read_mac (not esp_wifi_get_mac) so this works before the Wi-Fi
  // driver has been started — BLE comes up first in this flow.
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  const String serviceName = deviceServiceName(mac);
  gServiceName = serviceName;

  // Fresh 32-byte token per session (§8). Never persisted, invalid once this
  // function returns.
  uint8_t token[prov_crypto::kTokenLen];
  esp_fill_random(token, sizeof(token));

  // Two keys, one per direction (§4) — this is what makes a nonce collision
  // between directions structurally impossible.
  if (!prov_crypto::hkdfSha256(token, sizeof(token), mac, sizeof(mac), "esp32c6-prov-v2 c2d",
                               gSession.kC2D, prov_crypto::kKeyLen) ||
      !prov_crypto::hkdfSha256(token, sizeof(token), mac, sizeof(mac), "esp32c6-prov-v2 d2c",
                               gSession.kD2C, prov_crypto::kKeyLen)) {
    Serial.println("[Prov] HKDF failed");
    return false;
  }

  BLEDevice::init(serviceName.c_str());
  BLEDevice::setMTU(517);  // request; fragmentation copes if the peer says no

  BLEServer *server = BLEDevice::createServer();
  server->setCallbacks(new ServerCallbacks());

  BLEService *service = server->createService(kServiceUuid);

  // Both WRITE and WRITE_NR: Web Bluetooth clients reach for
  // writeValueWithoutResponse() by default, and a characteristic that only
  // advertises WRITE rejects it — the write never arrives and the device
  // looks silent rather than erroring.
  BLECharacteristic *rxChar = service->createCharacteristic(
      kRxCharUuid, BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
  rxChar->setCallbacks(new RxCallbacks());

  // No explicit BLE2902/CCCD descriptor: NimBLE adds it automatically for a
  // characteristic with NOTIFY, and the manual class is deprecated. Adding
  // one by hand also gives a descriptor callback that never fires, which is
  // a trap if any logic depends on observing the subscription.
  gTxChar = service->createCharacteristic(kTxCharUuid, BLECharacteristic::PROPERTY_NOTIFY);

  service->start();

  BLEAdvertising *advertising = BLEDevice::getAdvertising();
  advertising->addServiceUUID(kServiceUuid);
  advertising->setScanResponse(true);
  BLEDevice::startAdvertising();

  char macHex[13];
  snprintf(macHex, sizeof(macHex), "%02x%02x%02x%02x%02x%02x", mac[0], mac[1], mac[2], mac[3],
           mac[4], mac[5]);

  // Compact form "P2:<mac-hex>:<token-b64url>" rather than JSON
  // (PROVISIONING_PROTOCOL_V2.md §8). This is not cosmetic: the payload
  // length sets the QR version, which sets the module count, which decides
  // whether a camera can resolve it at all. JSON cost 108 bytes -> QR
  // version 7 (45x45) -> 2 physical pixels per module on this panel, below
  // what any scanner reliably reads. This is 59 bytes -> version 4 (33x33)
  // -> 4px per module.
  //
  // `name` is not carried because it is derivable: PROV_ + the last three
  // MAC bytes, uppercase. Repeating it cost ~22 bytes of a budget measured
  // in module size.
  //
  // ':' is a safe delimiter — base64url's alphabet is [A-Za-z0-9-_] and hex
  // is [0-9a-f], so neither field can contain one.
  String qr = String("P2:") + macHex + ":" + prov_crypto::base64UrlEncode(token, sizeof(token));

  Serial.println("[Prov] advertising as " + serviceName);
  Serial.println("[Prov] QR: " + qr);
  // Session secrets in the clear on the serial console. Acceptable only
  // because this is a bring-up POC and the token is single-use and already
  // displayed on screen — but this must not survive into production
  // firmware. Printed so a client that fails to authenticate can be compared
  // key-for-key instead of guessed at.
  Serial.println("[Prov] k_c2d: " + toHex(gSession.kC2D, prov_crypto::kKeyLen));
  Serial.println("[Prov] k_d2c: " + toHex(gSession.kD2C, prov_crypto::kKeyLen));
  if (onQrReady) onQrReady(qr);

  const uint32_t start = millis();
  bool success = false;

  while ((millis() - start) < timeoutMs) {
    // Replies enqueued by BLE callbacks go out here, on the app task — see
    // the outbound-queue note above. Drained first so a HELLO_ACK is not
    // held up behind a Wi-Fi or MQTT connect attempt.
    drainTxQueue();

    // The QR has been consumed by now, so the caller can drop it and show
    // progress instead.
    if (gHelloPending) {
      gHelloPending = false;
      report("Paired\n\nWaiting for Wi-Fi details...");
    }

    if (gWifiCredsPending) {
      gWifiCredsPending = false;
      const String ssid = gPendingSsid;
      const String pass = gPendingWifiPassword;
      Serial.println("[Prov] WIFI_CREDS for SSID '" + ssid + "'");
      report("Connecting to Wi-Fi\n\n" + ssid);

      JsonDocument st;
      st["state"] = "connecting";
      sendJson(kMsgWifiStatus, st);

      WifiResult wifi = netConnectWifi(ssid, pass, 20000, onTick);
      gWifiConnected = wifi.connected;
      if (wifi.connected) persistWifiCredentials(ssid, pass);
      report(wifi.connected ? ("Wi-Fi connected\n\n" + wifi.ip)
                            : ("Wi-Fi failed\n\n" + wifi.errorReason));

      JsonDocument done;
      done["state"] = wifi.connected ? "connected" : "failed";
      if (wifi.connected) {
        done["ip"] = wifi.ip;
      } else {
        done["reason"] = wifi.errorReason;
      }
      sendJson(kMsgWifiStatus, done);
      Serial.println("[Prov] Wi-Fi " + String(wifi.connected ? "connected" : "failed"));
    }

    if (gMqttConfigPending) {
      gMqttConfigPending = false;
      Serial.println("[Prov] MQTT_CONFIG for " + gPendingMqttConfig.brokerHost);
      report("Connecting to MQTT\n\n" + gPendingMqttConfig.brokerHost);

      JsonDocument st;
      st["state"] = "connecting";
      sendJson(kMsgMqttStatus, st);

      MqttResult mqtt = netConnectMqttAndTestEcho(
          gPendingMqttConfig.brokerHost, gPendingMqttConfig.brokerPort,
          gPendingMqttConfig.useTls, gPendingMqttConfig.clientId, gPendingMqttConfig.username,
          gPendingMqttConfig.password, 15000, onTick);

      JsonDocument done;
      done["state"] = mqtt.connected ? "connected" : "failed";
      if (!mqtt.connected) done["reason"] = mqtt.errorReason;
      sendJson(kMsgMqttStatus, done);

      success = mqtt.connected;
      // Terminal either way (§9 step 9) — the app is not going to send
      // anything further, so stop rather than idling out the 3-minute window.
      break;
    }

    if (onTick) onTick();
    delay(20);
  }

  // Ordered BLE teardown. Getting this wrong crashes the device rather than
  // leaking: BLEDevice::deinit() frees the BLEServer and its peer map from
  // THIS task, while the NimBLE host task may still be inside
  // handleGATTServerEvent() -> removePeerDevice() for the disconnect that
  // just happened. That race is a use-after-free on the peer map, and it
  // surfaces as "CORRUPT HEAP: ... got 0xfefefefe" (the freed-memory poison
  // pattern) or a load access fault deep in std::map, several frames away
  // from anything of ours.
  //
  // So: stop attracting new clients, hang up on the current one, wait for
  // the disconnect to actually land, and only then free anything.
  gShuttingDown = true;
  BLEDevice::stopAdvertising();

  if (gClientConnected) {
    Serial.println("[Prov] disconnecting BLE client before teardown");
    server->disconnect(server->getConnId());
    const uint32_t disconnectStart = millis();
    while (gClientConnected && (millis() - disconnectStart) < 1000) {
      delay(10);
    }
  }

  // Even after onDisconnect() fires, the host task still has queued events
  // to retire (ble_gap_conn_broken and friends) that touch the objects
  // deinit() is about to free. Give it room.
  delay(300);
  // false: keep the stack's memory so the phone remote (phone_hid.cpp) can
  // bring BLE back up later in this same boot.
  BLEDevice::deinit(false);
  delay(100);
  gTxChar = nullptr;
  gShuttingDown = false;

  // The token and both session keys die with this function. Wipe rather than
  // rely on the stack being reused, so a later heap dump can't recover them.
  memset(token, 0, sizeof(token));
  memset(&gSession, 0, sizeof(gSession));

  if (success) {
    persistMqttConfig(gPendingMqttConfig);
    mqttConfigOut = gPendingMqttConfig;
  }
  return success;
}
