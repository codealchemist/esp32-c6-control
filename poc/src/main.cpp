// ESP32-C6 Remote Control Panel — Proof of Concept
// Implements the sequence in ../docs/POC.md §2. See that document for the
// full flow diagram, rationale, and pass/fail checklist; see README.md in
// this directory for one-time manual setup steps before this will build.
//
// Lower-risk than provisioning.cpp, but still unverified on real hardware:
// the lv_qrcode_*/lv_list_* calls below assume LVGL v9's current widget
// API; also requires LV_USE_QRCODE enabled in lv_conf.h (README.md).

#include <Arduino.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <WiFi.h>
#include <lvgl.h>

#include "config_store.h"
#include "device_info.h"
#include "display.h"
#include "generated/icon_registry_gen.h"
#include "input.h"
#include "net.h"
#include "phone_hid.h"
#include "pins.h"
#include "provisioning.h"
#include "touch.h"
#include "ui_layout.h"
#include "app.h"
#include "menu.h"

namespace {

// Serial escape hatch for hardware without the dedicated button wired up.
//
// Once provisioning succeeds, loop() skips the QR entirely on every later
// boot (provisioningIsProvisioned()), and the only documented way back is
// holding the button at boot — which is no help if that button doesn't exist
// yet. Typing 'r' in the serial monitor clears the provisioning state and
// restarts, landing back on the QR.
//
// Restarting rather than just clearing the flag: the sequence has already
// read provisioningIsProvisioned() by the time most of these screens are
// showing, and half-applying the change mid-sequence would leave Wi-Fi and
// NVS state disagreeing about whether this device is provisioned.


// One line every 30s that tells "the board hung" apart from "touch stopped
// reporting" apart from "the screen loop isn't reading input": if these keep
// coming, the app task is alive; touch ok/err and last-touch age show the
// touch path; the screen name shows where the app is.
constexpr uint32_t kDiagEveryMs = 30000;
uint32_t gLastDiag = 0;

void diagPoll() {
  if (millis() - gLastDiag < kDiagEveryMs) return;
  gLastDiag = millis();
  const TouchStats t = touchStats();
  Serial.printf("[Diag] up=%lus screen=%s heap=%uKB min=%uKB touch ok=%lu err=%lu last=%s\n",
                millis() / 1000, uiScreenName(), ESP.getFreeHeap() / 1024,
                ESP.getMinFreeHeap() / 1024, (unsigned long)t.readsOk,
                (unsigned long)t.readErrors,
                t.msSinceTouch == UINT32_MAX ? "never"
                                             : (String(t.msSinceTouch / 1000) + "s ago").c_str());
}

void serialCommandPoll() {
  diagPoll();
  while (Serial.available() > 0) {
    const int c = Serial.read();
    // Serial stand-ins for the encoder, BOOT and touch actions, so every
    // screen is drivable from a console on boards without input wired:
    //   + / -  encoder turn      s  encoder push (open / set)
    //   b      BOOT (back)       a  home: connection details
    //   v / V  drag the volume slider right / left (simulated touch)
    //   h / H  swipe left / right (simulated touch)
    //   C      tap the Volume page's phone/computer switch (simulated touch)
    if (c == '+') {
      inputInject(InputEvent::EncoderCW);
    } else if (c == '-') {
      inputInject(InputEvent::EncoderCCW);
    } else if (c == 'a' || c == 'A') {
      inputInject(InputEvent::SecondaryAction);
    } else if (c == 'b' || c == 'B') {
      inputInject(InputEvent::ButtonPressShort);
    } else if (c == 's' || c == 'S') {
      inputInject(InputEvent::EncoderPressShort);
    } else if (c == 'v') {
      displaySimulateDrag(60, 129, 260, 129, 700);  // volume slider, left -> right
    } else if (c == 'V') {
      displaySimulateDrag(260, 129, 60, 129, 700);  // volume slider, right -> left
    } else if (c == 'h') {
      displaySimulateDrag(260, 70, 60, 70, 200);  // swipe left (next page)
    } else if (c == 'H') {
      displaySimulateDrag(60, 70, 260, 70, 200);  // swipe right (previous / back)
    } else if (c == 'C') {
      displaySimulateDrag(285, 26, 285, 26, 60);  // tap: Volume page target switch
    } else if (c == 't' || c == 'T') {
      touchSetDebug(!touchDebugEnabled());
    } else if (c == 'w' || c == 'W') {
      // What the board itself can see — the C6 is 2.4 GHz only, so this is
      // the quickest way to tell "wrong SSID" from "network is 5 GHz-only"
      // when provisioning reports "SSID not found".
      Serial.println("[Serial] 'w' — scanning Wi-Fi (2.4 GHz)...");
      if (WiFi.getMode() == WIFI_OFF) WiFi.mode(WIFI_STA);
      const int n = WiFi.scanNetworks();
      for (int i = 0; i < n; i++) {
        Serial.printf("[Scan] %-32s ch %2d  %4d dBm\n", WiFi.SSID(i).c_str(), WiFi.channel(i),
                      WiFi.RSSI(i));
      }
      Serial.printf("[Scan] %d network(s)\n", n < 0 ? 0 : n);
      WiFi.scanDelete();
    } else if (c == 'r' || c == 'R') {
      Serial.println("[Serial] 'r' — clearing provisioning state, restarting...");
      provisioningForceReset();
      Serial.flush();  // ESP.restart() does not drain the TX buffer for us.
      delay(50);
      ESP.restart();
    }
  }
}

void pumpFor(uint32_t ms) {
  uint32_t start = millis();
  while (millis() - start < ms) {
    displayTick();
    serialCommandPoll();
    delay(5);
  }
}

// How long a screen waits for input before advancing on its own. Exists so
// the sequence is walkable on hardware with no encoder or button wired up
// yet — the QR provisioning step is reachable without any input device.
constexpr uint32_t kIdleAdvanceMs = 5000;

// Blocks, pumping the display, until inputPoll() returns one of the given
// events (or, if acceptAny is true, any non-None event). Simple by design —
// see input.h's note on this being a depth-1 queue, fine for a linear POC.
//
// timeoutMs == 0 waits forever. Otherwise returns InputEvent::None once the
// timeout elapses, so callers can tell "user acted" from "gave up waiting".
InputEvent waitForInput(bool acceptAny, InputEvent only = InputEvent::None,
                         uint32_t timeoutMs = 0) {
  const uint32_t start = millis();
  while (true) {
    displayTick();
    serialCommandPoll();
    InputEvent ev = inputPoll();
    if (ev != InputEvent::None && (acceptAny || ev == only)) {
      return ev;
    }
    // Unsigned arithmetic, so this stays correct across millis() rollover.
    if (timeoutMs != 0 && (millis() - start) >= timeoutMs) {
      return InputEvent::None;
    }
    delay(5);
  }
}

void littleFsSmokeTest() {
  Serial.println("[LittleFS] mounting...");
  if (!LittleFS.begin(true)) {
    Serial.println("[LittleFS] mount FAILED");
    return;
  }
  File f = LittleFS.open("/icons_manifest.json", "r");
  if (f) {
    Serial.printf("[LittleFS] /icons_manifest.json: %u bytes\n", f.size());
    f.close();
  } else {
    Serial.println("[LittleFS] /icons_manifest.json not found (did you run `pio run -t uploadfs`?)");
  }
}

// The connecting screen is rebuilt only when it is not already showing, so
// the spinner keeps turning across phase updates. Tracked by screen object,
// not by label pointer: any other screen being shown deletes this one.
lv_obj_t *gConnectingScreen = nullptr;
lv_obj_t *gConnectingLabel = nullptr;

lv_obj_t *gWelcomeScreen = nullptr;

void showWelcomeScreen() {
  gWelcomeScreen = uiBlankScreen("welcome");
  lv_obj_t *label = lv_label_create(gWelcomeScreen);
  lv_label_set_text(label, "ESP32-C6\nRemote Control\nPOC");
  lv_obj_align(label, LV_ALIGN_LEFT_MID, 100, 0);
}

void addIconToWelcomeScreen() {
  lv_obj_t *img = lv_image_create(gWelcomeScreen);
  // Which icon this is comes from icons_manifest.json's "welcomeIcon" —
  // changing it is a manifest edit, not a code edit.
  lv_image_set_src(img, kWelcomeIcon);
  lv_obj_align(img, LV_ALIGN_LEFT_MID, 16, 0);
}

void showDeviceInfoScreen() {
  // One action: Next on the left, the scrollable info list on the right.
  UiView v = uiShow("device-info", nullptr, nullptr, lv_color_white(),
                    {{"Next", InputEvent::EncoderPressShort}});
  lv_obj_t *list = lv_list_create(v.content);
  lv_obj_set_size(list, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_pad_all(list, 0, 0);
  lv_obj_set_style_border_width(list, 0, 0);

  for (const String &line : collectDeviceInfoLines()) {
    lv_list_add_text(list, line.c_str());
  }
  // Without input hardware nothing else says the screen moves on by itself,
  // which reads as a hang.
  lv_list_add_text(list, ("Continues after " + String(kIdleAdvanceMs / 1000) + "s idle").c_str());

  Serial.printf("[DeviceInfo] scroll by touch or encoder, Next/push to continue "
                 "(auto-advances after %ums idle)\n",
                 kIdleAdvanceMs);

  // Idle timer, not a fixed deadline: scrolling restarts it.
  uint32_t lastActivity = millis();
  while (true) {
    displayTick();
    serialCommandPoll();
    InputEvent ev = inputPoll();
    if (ev == InputEvent::EncoderCW) {
      lv_obj_scroll_by(list, 0, -24, LV_ANIM_ON);
      lastActivity = millis();
    } else if (ev == InputEvent::EncoderCCW) {
      lv_obj_scroll_by(list, 0, 24, LV_ANIM_ON);
      lastActivity = millis();
    } else if (ev == InputEvent::EncoderPressShort || ev == InputEvent::EncoderPressLong ||
               ev == InputEvent::ButtonPressShort) {
      break;
    } else if ((millis() - lastActivity) >= kIdleAdvanceMs &&
               lv_display_get_inactive_time(nullptr) >= kIdleAdvanceMs) {
      // Second condition: a finger dragging the list counts as activity too.
      Serial.println("[DeviceInfo] idle timeout — advancing");
      break;
    }
    delay(5);
  }
}

// ARCHITECTURE.md §5.2's `"showAbout"` action, in the reduced form this POC
// can support. A licence obligation, not decoration: Icons8's free tier
// (§12.1) requires a visible credit. The text comes from the manifest via
// kIconAttribution, so the credit cannot drift from the licence record.
void showAboutScreen() {
  uiShow("about",
         (String("About\n") + kIconAttribution + "\n(continues in " +
          String(kIdleAdvanceMs / 1000) + "s)")
             .c_str(),
         LV_SYMBOL_LIST, lv_color_white(), {{"OK", InputEvent::EncoderPressShort}});

  Serial.println("[About] " + String(kIconAttribution));

  // Timed out too: without input hardware this would otherwise stand between
  // boot and the provisioning QR.
  waitForInput(/*acceptAny=*/true, InputEvent::None, kIdleAdvanceMs);
}

void showProvisioningScreen(const String &qrPayload) {
  lv_obj_t *scr = uiBlankScreen("pairing-qr");

  // Dim while the QR is up — see kBrightnessQr in display.h. Restored by
  // loop() once provisioningRun() returns.
  displaySetBrightness(kBrightnessQr);

  lv_obj_t *qr = lv_qrcode_create(scr);
  // 164px gives the compact payload (PROVISIONING_PROTOCOL_V2.md §8) 4px per
  // module: the practical ceiling on this 172px panel, and what cameras need.
  lv_qrcode_set_size(qr, 164);
  // Deliberately NOT themed: dark-on-light even though every other surface
  // is dark — inverted codes are out of spec and many scanners refuse them.
  lv_qrcode_set_dark_color(qr, lv_color_black());
  lv_qrcode_set_light_color(qr, lv_color_white());
  // Scanners need ~4 modules of light margin; the dark screen gives none.
  lv_qrcode_set_quiet_zone(qr, true);
  lv_qrcode_update(qr, qrPayload.c_str(), qrPayload.length());
  lv_obj_align(qr, LV_ALIGN_LEFT_MID, 4, 0);

  lv_obj_t *label = lv_label_create(scr);
  lv_label_set_text(label, "Scan to pair");
  lv_obj_set_width(label, 138);
  lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(label, LV_ALIGN_RIGHT_MID, -6, 0);

  Serial.println("[Provisioning] QR ready:");
  Serial.println(qrPayload);

  displayTick();
}

// A spinner in the big-icon slot plus whatever phase is running. Built once
// and only re-texted afterwards — rebuilding per update would restart the
// spinner animation and make it stutter on every phase change.
void showConnectingScreen(const String &status) {
  if (gConnectingScreen == nullptr || lv_screen_active() != gConnectingScreen) {
    gConnectingScreen = uiBlankScreen("connecting");

    lv_obj_t *spinner = lv_spinner_create(gConnectingScreen);
    lv_obj_set_size(spinner, 64, 64);
    lv_spinner_set_anim_params(spinner, 1000, 60);
    lv_obj_align(spinner, LV_ALIGN_LEFT_MID, 12, 0);

    gConnectingLabel = lv_label_create(gConnectingScreen);
    lv_obj_set_width(gConnectingLabel, 220);
    lv_label_set_long_mode(gConnectingLabel, LV_LABEL_LONG_WRAP);
    lv_obj_align(gConnectingLabel, LV_ALIGN_RIGHT_MID, -6, 0);

    // The QR is spent, so nothing needs the reduced backlight any more.
    displaySetBrightness(kBrightnessNormal);
  }

  lv_label_set_text(gConnectingLabel, status.c_str());
  Serial.println("[Connecting] " + status);
  displayTick();
}

// --- Online service ---------------------------------------------------------------
//
// Everything the device does while connected, independent of which screen is
// showing (nodrige docs/00-poc-implementation-guide.md §0.6 items 6–7, and
// docs/ARCHITECTURE.md §5.3 point 3): incoming commands
// and config pushes. Called from every online screen's loop.

// MQTT broker this boot connected to, for the device details page.
String gBrokerHost;

// Set while the MQTT client is up with device topics subscribed; gates the
// online part of appPump().
bool gOnline = false;

// Replies to menu tasks (task_result), held until whoever waits claims them
// by requestId (appTakeTaskResult). A few slots, since several tiles can be
// waiting at once; the oldest is overwritten.
struct TaskResult {
  String id, json;
};
TaskResult gTaskResults[4];
size_t gTaskResultNext = 0;


void onlineService() {
  String cmd;
  if (netMqttTakeCommand(cmd)) {
    Serial.println("[Command] " + cmd);
    JsonDocument doc;
    if (deserializeJson(doc, cmd) != DeserializationError::Ok) {
      Serial.println("[Command] not JSON - ignored");
    } else if (String(doc["action"] | "") == "scene_state") {
      menuSetActiveScenes(doc["active"].as<JsonArrayConst>());
    } else if (String(doc["action"] | "") == "task_result") {
      // A reply to a menu task (§6.3), matched by requestId by whoever waits.
      gTaskResults[gTaskResultNext] = {doc["correlationId"] | "", cmd};
      gTaskResultNext = (gTaskResultNext + 1) % 4;
    }
  }

  String pushed;
  if (netMqttTakeConfig(pushed)) {
    Serial.printf("[Config] push received (%u bytes)\n", pushed.length());
    String result;
    const PushResult r = configApplyPushed(pushed, result);
    const bool sent = netMqttPublishResult(result);
    Serial.println("[Config] result " + result + (sent ? "" : " — NOT sent"));
    if (r == PushResult::Applied) {
      // §5.3: always a full reboot to apply. Show why, and give the QoS 1
      // result publish a moment to leave before the connection drops.
      uiShow("config-applied", "Config updated\nRestarting...", LV_SYMBOL_REFRESH,
             lv_palette_main(LV_PALETTE_BLUE));
      pumpFor(1500);
      Serial.flush();
      ESP.restart();
    }
  }
}

// Pairing failed or timed out: say so, then retry pairing (press Retry, or
// wait 30s).
void showStatusScreen(const String &summary) {
  uiShow("status", summary.c_str(), LV_SYMBOL_WARNING, lv_palette_main(LV_PALETTE_RED),
         {{"Retry", InputEvent::EncoderPressShort}});
  Serial.println("[Status] retrying in 30s (or press Retry)");
  waitForInput(/*acceptAny=*/true, InputEvent::None, 30000);
}

} // namespace

void appPump() {
  displayTick();
  netWifiPoll();
  phoneHidPoll();
  serialCommandPoll();
  if (gOnline) onlineService();
}

const String &appBrokerHost() { return gBrokerHost; }

bool appTakeTaskResult(const String &requestId, String &resultJson) {
  for (TaskResult &r : gTaskResults) {
    if (!r.id.isEmpty() && r.id == requestId) {
      resultJson = r.json;
      r = {};
      return true;
    }
  }
  return false;
}

void setup() {
  Serial.begin(115200);
  // Just long enough for a USB serial monitor to attach and catch the banner;
  // the screen comes up right after config load.
  delay(300);
  Serial.println("=== ESP32-C6 Remote Control Panel POC ===");
  Serial.println("[Serial] type 'r' + Enter at any time to clear provisioning "
                  "and restart (re-shows the pairing QR)");

  littleFsSmokeTest();

  // docs/ARCHITECTURE.md §5.3 boot step: load /config/active.json, creating
  // it from the burned-in default if missing or invalid.
  configLoad();

  // Verifies this build's crypto against the spec's published test vectors
  // (PROVISIONING_PROTOCOL_V2.md §10) before anything depends on it.
  provisioningSelfTest();

  displayInit();
  inputInit();

  // Boot-time button hold forces re-provisioning even if already
  // provisioned — docs/POC.md §3.
  if (inputButtonHeldNow()) {
    Serial.println("[Boot] button held — forcing re-provisioning");
    provisioningForceReset();
  }

  // Hang detector: from here on, displayTick() (called by every wait loop)
  // feeds the loop watchdog; 5s without it panics with a backtrace.
  enableLoopWDT();
}

void loop() {
  // The intro (welcome, device info, About credit) only matters before the
  // board is paired. Once it is, boot goes straight to connecting: the same
  // details live on the home screen's device details page, and the Icons8
  // credit stays reachable under Settings > About (ARCHITECTURE.md §12.1).
  static bool firstPass = true;
  if (!provisioningIsProvisioned() && firstPass) {
    showWelcomeScreen();
    pumpFor(2000);
    addIconToWelcomeScreen();
    pumpFor(2000);

    showDeviceInfoScreen();
    showAboutScreen();
  }
  firstPass = false;

  MqttProvisionedConfig mqttCfg;

  if (provisioningIsProvisioned()) {
    Serial.println("[Provisioning] already provisioned, skipping BLE pairing");
    mqttCfg = provisioningLoadStoredMqttConfig();
  } else {
    bool ok = provisioningRun([](const String &qr) { showProvisioningScreen(qr); },
                               [](const String &status) { showConnectingScreen(status); },
                               []() { displayTick(); }, mqttCfg, 180000 /* 3 min */);
    // Unconditional: showProvisioningScreen() dimmed the backlight, and every
    // path out of provisioningRun() — success, failure, timeout — leaves the
    // QR screen behind.
    displaySetBrightness(kBrightnessNormal);
    if (!ok) {
      Serial.println("[Status] Wi-Fi: provisioning failed or timed out");
      showStatusScreen("Pairing failed\nor timed out");
      return; // loop() re-enters from the top
    }
  }

  // Fill active.json's mqtt block from provisioning (§5.1) — a no-op when
  // nothing changed since the last boot.
  configSyncMqtt(mqttCfg);

  // Credentials come from our own NVS copy, NOT from WiFi.SSID()/psk():
  // those report the currently associated AP, which on a fresh boot is
  // nothing at all (see provisioningLoadStoredWifi).
  const WifiCredentials wifiCreds = provisioningLoadStoredWifi();

  // First Wi-Fi attempt in the foreground, with the spinner, so a normal boot
  // reaches the menu already connected. Either way the menu opens after it:
  // Wi-Fi and the server keep retrying in the background, and what needs the
  // Core is shown disabled until they're back.
  showConnectingScreen("Connecting to Wi-Fi\n\n" + wifiCreds.ssid);
  WifiResult wifi =
      netConnectWifi(wifiCreds.ssid, wifiCreds.password, 8000, []() { displayTick(); });
  Serial.println("[Status] " + String(wifi.connected ? "Wi-Fi: " + wifiCreds.ssid +
                                                           " - Connected (" + wifi.ip + ")"
                                                     : "Wi-Fi: Failed (" + wifi.errorReason +
                                                           ") - retrying in the background"));
  netWifiKeepConnected(wifiCreds.ssid, wifiCreds.password);

  if (mqttCfg.valid) {
    gBrokerHost = mqttCfg.brokerHost;
    DeviceTopics topics;
    topics.commands = mqttCfg.topicCommands;
    topics.messages = mqttCfg.topicMessages;
    topics.config = mqttCfg.topicConfig;
    topics.commandResults = mqttCfg.topicCommandResults;
    if (!netMqttBegin(mqttCfg.brokerHost, mqttCfg.brokerPort, mqttCfg.useTls, mqttCfg.clientId,
                      mqttCfg.username, mqttCfg.password, topics)) {
      Serial.println("[MQTT] stored config unusable (no host or topics) - staying offline");
    } else if (wifi.connected) {
      // A moment for the usual case, so home opens already online.
      showConnectingScreen("Connecting to server\n\n" + mqttCfg.brokerHost);
      const uint32_t start = millis();
      while (!netMqttIsConnected() && millis() - start < 4000) {
        displayTick();
        serialCommandPoll();
        delay(10);
      }
      Serial.println(netMqttIsConnected() ? "[Status] MQTT: Connected"
                                          : "[Status] MQTT: not reachable yet - retrying in the background");
    }
  } else {
    Serial.println("[MQTT] no stored broker config - staying offline");
  }

  gOnline = true;  // the online services run from here on; they cope with being offline
  // The phone remote (Volume menu item). BLE comes up only now: provisioning
  // owns it before this, and starting it once per boot is enough.
  static String hidName = "Panel-" + WiFi.macAddress().substring(12, 14) +
                          WiFi.macAddress().substring(15, 17);
  phoneHidBegin(hidName.c_str());
  menuHome();  // never returns
}
