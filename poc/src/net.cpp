#include "net.h"

#include <WiFi.h>
#include <mqtt_client.h>

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
    result.errorReason = (WiFi.status() == WL_NO_SSID_AVAIL) ? "SSID not found"
                          : (WiFi.status() == WL_CONNECT_FAILED)
                              ? "auth failed"
                              : "timeout";
  }
  return result;
}

namespace {

volatile bool gMqttConnected = false;
volatile bool gMqttError = false;
volatile bool gMqttEchoReceived = false;
String gEchoTopic;

void mqttEventHandler(void *handler_args, esp_event_base_t base, int32_t event_id,
                       void *event_data) {
  auto *event = static_cast<esp_mqtt_event_handle_t>(event_data);
  switch (static_cast<esp_mqtt_event_id_t>(event_id)) {
    case MQTT_EVENT_CONNECTED:
      gMqttConnected = true;
      esp_mqtt_client_subscribe(event->client, gEchoTopic.c_str(), 1);
      break;
    case MQTT_EVENT_ERROR:
      gMqttError = true;
      break;
    case MQTT_EVENT_DATA:
      if (gEchoTopic.length() > 0 &&
          strncmp(event->topic, gEchoTopic.c_str(), event->topic_len) == 0) {
        gMqttEchoReceived = true;
      }
      break;
    default:
      break;
  }
}

} // namespace

MqttResult netConnectMqttAndTestEcho(const String &host, uint16_t port, bool useTls,
                                      const String &clientId, const String &username,
                                      const String &password, uint32_t timeoutMs,
                                      const std::function<void()> &onPoll) {
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

  esp_mqtt_client_handle_t client = esp_mqtt_client_init(&cfg);
  if (!client) {
    result.errorReason = "client init failed";
    return result;
  }
  esp_mqtt_client_register_event(client, MQTT_EVENT_ANY, mqttEventHandler, nullptr);
  esp_mqtt_client_start(client);

  const uint32_t start = millis();
  while (!gMqttConnected && !gMqttError && (millis() - start) < timeoutMs) {
    if (onPoll) onPoll();
    delay(20);
  }

  if (!gMqttConnected) {
    result.connected = false;
    result.errorReason = gMqttError ? "connect error (bad host/port/credentials?)" : "timeout";
    esp_mqtt_client_stop(client);
    esp_mqtt_client_destroy(client);
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
