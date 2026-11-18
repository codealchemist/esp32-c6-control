#include "net.h"

#include <WiFi.h>
#include <mqtt_client.h>

namespace {

// Last STA disconnect reason from the Wi-Fi driver. WiFi.status() collapses
// most failures into WL_DISCONNECTED/WL_IDLE_STATUS by the time the wait
// loop gives up, so the driver's own reason code is the only reliable way to
// tell "network not visible" from "wrong password".
volatile uint8_t gLastStaDisconnectReason = 0;
bool gWifiEventRegistered = false;

String describeWifiFailure(uint8_t reason) {
  switch (reason) {
    case WIFI_REASON_NO_AP_FOUND:
      return "SSID not found (check name; 2.4 GHz only)";
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_AUTH_EXPIRE:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_MIC_FAILURE:
      return "auth failed (wrong password?)";
    case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
    case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
      return "security mode not supported";
    case 0:
      return "timeout";
    default:
      return "timeout (reason " + String(reason) + ")";
  }
}

} // namespace

WifiResult netConnectWifi(const String &ssid, const String &password, uint32_t timeoutMs,
                           const std::function<void()> &onPoll) {
  WifiResult result;

  // Fail fast and honestly. WiFi.begin() rejects an empty SSID outright
  // ("SSID too long or missing!"), after which the wait below would burn the
  // full timeout and report "timeout" — pointing at the network rather than
  // at the missing credential that actually caused it.
  if (ssid.length() == 0) {
    result.connected = false;
    result.errorReason = "no SSID stored (re-provision needed)";
    return result;
  }

  // Already on this network — typically right after provisioning, which
  // joined it moments ago. Calling WiFi.begin() again would drop the link
  // and abort any MQTT connection riding on it, for nothing.
  if (WiFi.status() == WL_CONNECTED && WiFi.SSID() == ssid) {
    result.connected = true;
    result.ip = WiFi.localIP().toString();
    return result;
  }

  if (!gWifiEventRegistered) {
    WiFi.onEvent(
        [](arduino_event_id_t, arduino_event_info_t info) {
          const uint8_t reason = info.wifi_sta_disconnected.reason;
          // Each failed attempt ends with a teardown event (36, "STA
          // leaving"; 8, "assoc leave") that would overwrite the actual
          // cause, e.g. 201 NO_AP_FOUND. Keep the cause.
          if (reason == 36 || reason == WIFI_REASON_ASSOC_LEAVE) return;
          gLastStaDisconnectReason = reason;
        },
        ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
    gWifiEventRegistered = true;
  }
  gLastStaDisconnectReason = 0;

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid.c_str(), password.c_str());

  const uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - start) < timeoutMs) {
    // Short slices with a pump between them, so a caller's UI keeps
    // animating for the whole attempt instead of freezing on entry.
    if (onPoll) onPoll();
    delay(20);
  }

  if (WiFi.status() == WL_CONNECTED) {
    result.connected = true;
    result.ip = WiFi.localIP().toString();
  } else {
    result.connected = false;
    result.errorReason = describeWifiFailure(gLastStaDisconnectReason);
    // Without this the driver keeps auto-reconnecting to the bad network
    // every few seconds, indefinitely, long after this attempt has been
    // reported as failed — burning radio time BLE provisioning needs.
    WiFi.disconnect();
  }
  return result;
}

namespace {

// The one MQTT client this module owns (see net.h). nullptr when none.
esp_mqtt_client_handle_t gClient = nullptr;

volatile bool gMqttConnected = false;  // current link state, tracks reconnects
volatile bool gMqttError = false;
volatile bool gMqttEchoReceived = false;
String gEchoTopic;

// Device topics. Written only from the app task while no subscription is
// active for them yet, read from the MQTT task — Strings are not touched
// concurrently in practice.
String gCommandsTopic;
String gMessagesTopic;
String gConfigTopic;
String gResultsTopic;

// Config documents, reassembled from MQTT fragments in the MQTT task and
// handed to the app task. Only the MQTT task touches the assembly buffer;
// the hand-off pointer is swapped under gCmdMux.
constexpr size_t kMaxConfigLen = 8192;
char *gCfgAssembly = nullptr;
size_t gCfgAssemblyLen = 0;
char *gCfgReady = nullptr;  // complete, NUL-terminated, owned by whoever takes it

// Latest command, handed from the MQTT task to the app task.
// Fixed buffer, not a String: heap allocation is not allowed inside a
// portENTER_CRITICAL section.
// A small FIFO, not a single slot: a task reply and an echo can arrive back
// to back, and a slot would silently keep only the last one.
// Weather replies run ~700 bytes; commands arrive in one MQTT fragment as
// long as they fit the client buffer (set to 2 KB in netMqttBegin()).
constexpr size_t kMaxCommandLen = 1023;
constexpr uint8_t kCmdQueueLen = 4;
portMUX_TYPE gCmdMux = portMUX_INITIALIZER_UNLOCKED;
char gCmdQueue[kCmdQueueLen][kMaxCommandLen + 1];
uint8_t gCmdHead = 0, gCmdCount = 0;

bool topicEquals(const esp_mqtt_event_handle_t event, const String &topic) {
  return topic.length() > 0 && event->topic_len == static_cast<int>(topic.length()) &&
         memcmp(event->topic, topic.c_str(), event->topic_len) == 0;
}

void mqttEventHandler(void *handler_args, esp_event_base_t base, int32_t event_id,
                       void *event_data) {
  auto *event = static_cast<esp_mqtt_event_handle_t>(event_data);
  switch (static_cast<esp_mqtt_event_id_t>(event_id)) {
    case MQTT_EVENT_CONNECTED:
      gMqttConnected = true;
      // Subscriptions do not survive a reconnect on a clean session, so
      // (re)issue them on every CONNECTED, not just the first.
      if (gEchoTopic.length() > 0) esp_mqtt_client_subscribe(event->client, gEchoTopic.c_str(), 1);
      Serial.println("[MQTT] connected");
      if (gCommandsTopic.length() > 0) {
        esp_mqtt_client_subscribe(event->client, gCommandsTopic.c_str(), 1);
      }
      if (gConfigTopic.length() > 0) {
        esp_mqtt_client_subscribe(event->client, gConfigTopic.c_str(), 1);
      }
      break;
    case MQTT_EVENT_DISCONNECTED:
      if (gMqttConnected) Serial.println("[MQTT] disconnected - reconnecting in the background");
      gMqttConnected = false;
      break;
    case MQTT_EVENT_ERROR:
      gMqttError = true;
      break;
    case MQTT_EVENT_DATA: {
      // A message larger than the client buffer arrives as several DATA
      // events: only the first carries the topic; the rest carry
      // current_data_offset > 0 and no topic.
      const bool first = event->current_data_offset == 0;
      if (first && topicEquals(event, gConfigTopic)) {
        free(gCfgAssembly);
        gCfgAssembly = nullptr;
        if (event->total_data_len == 0) break;  // retained-message clear
        if (static_cast<size_t>(event->total_data_len) > kMaxConfigLen) {
          Serial.printf("[MQTT] config push of %d bytes exceeds %u — dropped\n",
                        event->total_data_len, kMaxConfigLen);
          break;
        }
        gCfgAssembly = static_cast<char *>(malloc(event->total_data_len + 1));
        gCfgAssemblyLen = event->total_data_len;
      }
      if (gCfgAssembly && (first ? topicEquals(event, gConfigTopic) : true) &&
          static_cast<size_t>(event->current_data_offset + event->data_len) <= gCfgAssemblyLen) {
        memcpy(gCfgAssembly + event->current_data_offset, event->data, event->data_len);
        if (static_cast<size_t>(event->current_data_offset + event->data_len) == gCfgAssemblyLen) {
          gCfgAssembly[gCfgAssemblyLen] = '\0';
          portENTER_CRITICAL(&gCmdMux);
          char *old = gCfgReady;
          gCfgReady = gCfgAssembly;
          portEXIT_CRITICAL(&gCmdMux);
          free(old);  // an unconsumed older push is superseded
          gCfgAssembly = nullptr;
        }
        break;
      }

      // Everything else: only whole, unfragmented messages.
      if (!first || event->topic_len == 0 || event->data_len != event->total_data_len) break;
      if (topicEquals(event, gEchoTopic)) {
        gMqttEchoReceived = true;
      } else if (topicEquals(event, gCommandsTopic)) {
        const size_t n = min(static_cast<size_t>(event->data_len), kMaxCommandLen);
        portENTER_CRITICAL(&gCmdMux);
        if (gCmdCount == kCmdQueueLen) {  // full: drop the oldest
          gCmdHead = (gCmdHead + 1) % kCmdQueueLen;
          gCmdCount--;
        }
        char *slot = gCmdQueue[(gCmdHead + gCmdCount) % kCmdQueueLen];
        memcpy(slot, event->data, n);
        slot[n] = '\0';
        gCmdCount++;
        portEXIT_CRITICAL(&gCmdMux);
      }
      break;
    }
    default:
      break;
  }
}

} // namespace

void netMqttStop() {
  if (gClient) {
    // Stopping a client that is mid-reconnect (broker unreachable) blocks
    // until its task notices — ~5s, which is the loop watchdog's limit, so
    // that path rebooted the board. Pause the watchdog for the stop only.
    disableLoopWDT();
    esp_mqtt_client_stop(gClient);
    esp_mqtt_client_destroy(gClient);
    enableLoopWDT();
    gClient = nullptr;
  }
  gMqttConnected = false;
  gCommandsTopic = "";
  gMessagesTopic = "";
  gConfigTopic = "";
  gResultsTopic = "";
  gCmdHead = 0;
  gCmdCount = 0;
  // The client (and its task) is gone, so nothing else touches these now.
  free(gCfgAssembly);
  gCfgAssembly = nullptr;
  free(gCfgReady);
  gCfgReady = nullptr;
}

MqttResult netConnectMqttAndTestEcho(const String &host, uint16_t port, bool useTls,
                                      const String &clientId, const String &username,
                                      const String &password, uint32_t timeoutMs,
                                      const std::function<void()> &onPoll) {
  // Never two clients at once — see net.h.
  netMqttStop();

  MqttResult result;
  gMqttConnected = false;
  gMqttError = false;
  gMqttEchoReceived = false;
  gEchoTopic = clientId + "/poc-echo";

  String uri = String(useTls ? "mqtts://" : "mqtt://") + host + ":" + String(port);

  esp_mqtt_client_config_t cfg = {};
  cfg.broker.address.uri = uri.c_str();
  cfg.credentials.client_id = clientId.c_str();
  if (username.length() > 0) {
    cfg.credentials.username = username.c_str();
  }
  if (password.length() > 0) {
    cfg.credentials.authentication.password = password.c_str();
  }
  if (useTls) {
    // POC simplification: skip certificate verification rather than pin a
    // CA cert. docs/ARCHITECTURE.md §8 requires cert pinning for the real
    // app — do not carry this flag forward past the POC.
    cfg.broker.verification.skip_cert_common_name_check = true;
    cfg.broker.verification.certificate = nullptr;
  }

  // esp_mqtt_client_init copies every string out of cfg, so the locals above
  // only need to outlive this call.
  esp_mqtt_client_handle_t client = esp_mqtt_client_init(&cfg);
  if (!client) {
    result.errorReason = "client init failed";
    return result;
  }
  esp_mqtt_client_register_event(client, MQTT_EVENT_ANY, mqttEventHandler, nullptr);
  esp_mqtt_client_start(client);
  gClient = client;

  const uint32_t start = millis();
  while (!gMqttConnected && !gMqttError && (millis() - start) < timeoutMs) {
    if (onPoll) onPoll();
    delay(20);
  }

  if (!gMqttConnected) {
    result.connected = false;
    result.errorReason = gMqttError ? "connect error (bad host/port/credentials?)" : "timeout";
    netMqttStop();
    return result;
  }

  result.connected = true;

  // Give the subscribe (fired in the CONNECTED handler) a moment to land
  // before publishing, then wait for the echo.
  delay(300);
  esp_mqtt_client_publish(client, gEchoTopic.c_str(), "hello", 0, 1, false);

  const uint32_t echoStart = millis();
  while (!gMqttEchoReceived && (millis() - echoStart) < 5000) {
    if (onPoll) onPoll();
    delay(20);
  }
  result.echoOk = gMqttEchoReceived;
  if (!result.echoOk) {
    result.errorReason = "connected, but no pub/sub echo (broker ACL or topic issue?)";
  }

  return result;
}

bool netMqttBegin(const String &host, uint16_t port, bool useTls, const String &clientId,
                  const String &username, const String &password, const DeviceTopics &t) {
  // Never two clients at once — see net.h.
  netMqttStop();
  if (host.length() == 0 || t.commands.length() == 0 || t.messages.length() == 0) return false;

  gMqttConnected = false;
  gMqttError = false;
  gEchoTopic = "";
  // Topics first: the CONNECTED handler subscribes to them, on the first
  // connect and every reconnect after.
  gCommandsTopic = t.commands;
  gMessagesTopic = t.messages;
  gConfigTopic = t.config;
  gResultsTopic = t.commandResults;

  const String uri = String(useTls ? "mqtts://" : "mqtt://") + host + ":" + String(port);
  esp_mqtt_client_config_t cfg = {};
  cfg.broker.address.uri = uri.c_str();
  cfg.credentials.client_id = clientId.c_str();
  if (username.length() > 0) cfg.credentials.username = username.c_str();
  if (password.length() > 0) cfg.credentials.authentication.password = password.c_str();
  if (useTls) {
    // POC simplification, as in netConnectMqttAndTestEcho().
    cfg.broker.verification.skip_cert_common_name_check = true;
    cfg.broker.verification.certificate = nullptr;
  }
  // Keep trying while the broker (or Wi-Fi) is away; the panel works offline
  // meanwhile and lights its remote features up again on connect.
  cfg.network.reconnect_timeout_ms = 5000;
  // Whole commands in one DATA event (only config pushes are reassembled).
  cfg.buffer.size = 2048;

  esp_mqtt_client_handle_t client = esp_mqtt_client_init(&cfg);
  if (!client) return false;
  esp_mqtt_client_register_event(client, MQTT_EVENT_ANY, mqttEventHandler, nullptr);
  esp_mqtt_client_start(client);
  gClient = client;
  Serial.println("[MQTT] client started for " + uri + " (connects/reconnects in the background)");
  return true;
}

// Background Wi-Fi: once given the credentials, re-issues the connect every
// kWifiRetryMs while not connected (the driver's own auto-reconnect only
// covers drops after a successful connect, not a failed first attempt).
namespace {
constexpr uint32_t kWifiRetryMs = 20000;
String gWifiSsid, gWifiPass;
uint32_t gWifiLastTry = 0;
bool gWifiWasConnected = false;
} // namespace

void netWifiKeepConnected(const String &ssid, const String &password) {
  gWifiSsid = ssid;
  gWifiPass = password;
  gWifiLastTry = millis();
  gWifiWasConnected = WiFi.status() == WL_CONNECTED;
  WiFi.setAutoReconnect(true);
}

void netWifiPoll() {
  if (gWifiSsid.length() == 0) return;
  const bool connected = WiFi.status() == WL_CONNECTED;
  if (connected != gWifiWasConnected) {
    gWifiWasConnected = connected;
    Serial.println(connected ? "[Wi-Fi] connected (" + WiFi.localIP().toString() + ")"
                             : String("[Wi-Fi] lost - retrying in the background"));
  }
  if (!connected && millis() - gWifiLastTry >= kWifiRetryMs) {
    gWifiLastTry = millis();
    WiFi.mode(WIFI_STA);
    WiFi.begin(gWifiSsid.c_str(), gWifiPass.c_str());  // returns at once
  }
}

bool netWifiConnected() { return WiFi.status() == WL_CONNECTED; }

bool netMqttStartDeviceTopics(const DeviceTopics &t) {
  if (!gClient || t.commands.length() == 0 || t.messages.length() == 0) return false;
  gCommandsTopic = t.commands;
  gMessagesTopic = t.messages;
  gConfigTopic = t.config;
  gResultsTopic = t.commandResults;
  // If currently disconnected, the CONNECTED handler subscribes on reconnect.
  if (gMqttConnected) {
    esp_mqtt_client_subscribe(gClient, gCommandsTopic.c_str(), 1);
    if (gConfigTopic.length() > 0) esp_mqtt_client_subscribe(gClient, gConfigTopic.c_str(), 1);
  }
  return true;
}

bool netMqttTakeCommand(String &payloadOut) {
  if (gCmdCount == 0) return false;
  char local[kMaxCommandLen + 1];
  portENTER_CRITICAL(&gCmdMux);
  memcpy(local, gCmdQueue[gCmdHead], sizeof(local));
  gCmdHead = (gCmdHead + 1) % kCmdQueueLen;
  gCmdCount--;
  portEXIT_CRITICAL(&gCmdMux);
  payloadOut = local;
  return true;
}

bool netMqttPublishEvent(const String &json, int qos) {
  if (!gClient || !gMqttConnected || gMessagesTopic.length() == 0) return false;
  return esp_mqtt_client_publish(gClient, gMessagesTopic.c_str(), json.c_str(), json.length(),
                                 qos, false) >= 0;
}

bool netMqttTakeConfig(String &payloadOut) {
  if (!gCfgReady) return false;
  portENTER_CRITICAL(&gCmdMux);
  char *doc = gCfgReady;
  gCfgReady = nullptr;
  portEXIT_CRITICAL(&gCmdMux);
  if (!doc) return false;
  payloadOut = doc;
  free(doc);
  return true;
}

bool netMqttPublishResult(const String &json) {
  if (!gClient || !gMqttConnected || gResultsTopic.length() == 0) return false;
  return esp_mqtt_client_publish(gClient, gResultsTopic.c_str(), json.c_str(), json.length(), 1,
                                 false) >= 0;
}

bool netMqttIsConnected() { return gClient != nullptr && gMqttConnected; }
