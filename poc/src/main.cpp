// ESP32-C6 Remote Control Panel — Proof of Concept
// Implements the sequence in ../docs/POC.md §2. See that document for the
// full flow diagram, rationale, and pass/fail checklist; see README.md in
// this directory for one-time manual setup steps before this will build.
//
// Lower-risk than provisioning.cpp, but still unverified on real hardware:
// the lv_qrcode_*/lv_list_* calls below assume LVGL v9's current widget
// API; also requires LV_USE_QRCODE enabled in lv_conf.h (README.md).

#include <Arduino.h>
#include <LittleFS.h>
#include <WiFi.h>
#include <lvgl.h>

#include "device_info.h"
#include "display.h"
#include "generated/icon_registry_gen.h"
#include "input.h"
#include "net.h"
#include "pins.h"
#include "provisioning.h"

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
void serialCommandPoll() {
  while (Serial.available() > 0) {
    const int c = Serial.read();
    if (c == 'r' || c == 'R') {
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

lv_obj_t *gConnectingLabel = nullptr;

lv_obj_t *makeBlankScreen() {
  lv_obj_t *scr = lv_obj_create(nullptr);
  lv_scr_load(scr);
  // Any widget pointer cached from the outgoing screen is dangling the
  // moment a new one loads. Cleared centrally so a stale pointer can never
  // outlive its screen.
  gConnectingLabel = nullptr;
  return scr;
}

lv_obj_t *gWelcomeScreen = nullptr;

void showWelcomeScreen() {
  gWelcomeScreen = makeBlankScreen();
  lv_obj_t *label = lv_label_create(gWelcomeScreen);
  lv_label_set_text(label, "ESP32-C6 Remote Control\nPOC");
  lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(label, LV_ALIGN_CENTER, 0, -20);
}

void addIconToWelcomeScreen() {
  lv_obj_t *img = lv_image_create(gWelcomeScreen);
  // Which icon this is comes from icons_manifest.json's "welcomeIcon" —
  // changing it is a manifest edit, not a code edit.
  lv_image_set_src(img, kWelcomeIcon);
  lv_obj_align(img, LV_ALIGN_CENTER, 0, 40);
}

void showDeviceInfoScreen() {
  lv_obj_t *scr = makeBlankScreen();
  lv_obj_t *list = lv_list_create(scr);
  lv_obj_set_size(list, LV_PCT(100), LV_PCT(100));

  for (const String &line : collectDeviceInfoLines()) {
    lv_list_add_text(list, line.c_str());
  }
  // On-screen equivalent of the Serial hint below — without an encoder wired
  // up there is otherwise nothing telling you the screen will move on by
  // itself, which reads as a hang.
  lv_list_add_text(list, ("-- push, or wait " + String(kIdleAdvanceMs / 1000) + "s --").c_str());

  Serial.printf("[DeviceInfo] scroll with the encoder, push to continue "
                 "(auto-advances after %ums idle)\n",
                 kIdleAdvanceMs);

  // Idle timer, not a fixed deadline: scrolling restarts it, so the timeout
  // only fires when nobody is actually using the screen. That keeps the
  // screen usable once an encoder IS wired up, rather than yanking it away
  // mid-scroll.
  uint32_t lastActivity = millis();
  while (true) {
    displayTick();
    serialCommandPoll();
    InputEvent ev = inputPoll();
    if (ev == InputEvent::EncoderCW) {
      lv_obj_scroll_by(list, 0, -20, LV_ANIM_ON);
      lastActivity = millis();
    } else if (ev == InputEvent::EncoderCCW) {
      lv_obj_scroll_by(list, 0, 20, LV_ANIM_ON);
      lastActivity = millis();
    } else if (ev == InputEvent::EncoderPressShort || ev == InputEvent::EncoderPressLong) {
      break;
    } else if ((millis() - lastActivity) >= kIdleAdvanceMs) {
      Serial.println("[DeviceInfo] idle timeout — advancing");
      break;
    }
    delay(5);
  }
}

// ARCHITECTURE.md §5.2's `"showAbout"` action, in the reduced form this POC
// can support: the real build reaches it from the Settings menu, which
// doesn't exist here, so it sits in the fixed boot sequence instead.
//
// This is a licence obligation, not decoration. ARCHITECTURE.md §12.1 chose
// Icons8's free tier, whose terms are conditional on a visible credit
// wherever the icons are used — and §12.2 leans on this screen harder still,
// since it is the project's only visible act of compliance. The text comes
// from the manifest via kIconAttribution rather than being written out here,
// so the credit on screen cannot drift from the one recorded next to the
// licence.
void showAboutScreen() {
  lv_obj_t *scr = makeBlankScreen();
  lv_obj_t *label = lv_label_create(scr);
  String text = String("About\n\n") + kIconAttribution +
                "\n\nPress any button, or wait " + String(kIdleAdvanceMs / 1000) + "s";
  lv_label_set_text(label, text.c_str());
  lv_obj_set_width(label, 300);
  lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(label, LV_ALIGN_CENTER, 0, 0);

  Serial.println("[About] " + String(kIconAttribution));

  // Also timed out: with no input hardware wired up this would otherwise be
  // the second thing standing between boot and the provisioning QR.
  waitForInput(/*acceptAny=*/true, InputEvent::None, kIdleAdvanceMs);
}

lv_obj_t *gProvisioningLabel = nullptr;

void showProvisioningScreen(const String &qrPayload) {
  lv_obj_t *scr = makeBlankScreen();

  // Dim while the QR is up — see kBrightnessQr in display.h. Restored by
  // loop() once provisioningRun() returns.
  displaySetBrightness(kBrightnessQr);

  lv_obj_t *qr = lv_qrcode_create(scr);
  // Sized to very nearly the full 172px screen height. Module size is what
  // decides whether a camera can resolve this at all, and it is the product
  // of canvas size and payload length: at 120px with the old JSON payload
  // each module was 2 physical pixels, which is below what any scanner can
  // reliably lock onto. 164px with the compact payload
  // (PROVISIONING_PROTOCOL_V2.md §8) gives 4px — the practical ceiling on a
  // 172px-tall panel, since 5px would need a 205px canvas.
  lv_qrcode_set_size(qr, 164);
  // Deliberately NOT themed: the QR stays dark-on-light even though every
  // other surface is dark. Inverted (light-on-dark) codes are out of spec and
  // a meaningful share of phone scanners refuse them, and a QR nobody can scan
  // fails provisioning outright.
  lv_qrcode_set_dark_color(qr, lv_color_black());
  lv_qrcode_set_light_color(qr, lv_color_white());
  // Matters much more now that the surrounding screen is dark. Off by default
  // (the widget's constructor never sets it), which lets the modules run to
  // the canvas edge — against a dark background that leaves no quiet zone at
  // all, and scanners need ~4 modules of light margin to lock on.
  lv_qrcode_set_quiet_zone(qr, true);
  lv_qrcode_update(qr, qrPayload.c_str(), qrPayload.length());
  lv_obj_align(qr, LV_ALIGN_LEFT_MID, 4, 0);

  // Whatever width is left beside the QR — deliberately the leftover, not a
  // round number, so growing the QR never silently overlaps the text.
  gProvisioningLabel = lv_label_create(scr);
  lv_label_set_text(gProvisioningLabel, "Scan to pair...");
  lv_obj_set_width(gProvisioningLabel, 138);
  lv_label_set_long_mode(gProvisioningLabel, LV_LABEL_LONG_WRAP);
  lv_obj_align(gProvisioningLabel, LV_ALIGN_RIGHT_MID, -6, 0);

  Serial.println("[Provisioning] QR ready:");
  Serial.println(qrPayload);

  displayTick();
}

// Replaces the QR once the app has paired: a spinner plus whatever phase
// provisioningRun() is currently in. Builds the screen on first call and
// only swaps the text afterwards — rebuilding per update would restart the
// spinner animation from zero and make it stutter on every phase change.
void showConnectingScreen(const String &status) {
  if (gConnectingLabel == nullptr) {
    lv_obj_t *scr = makeBlankScreen();

    lv_obj_t *spinner = lv_spinner_create(scr);
    lv_obj_set_size(spinner, 56, 56);
    lv_spinner_set_anim_params(spinner, 1000, 60);
    lv_obj_align(spinner, LV_ALIGN_LEFT_MID, 26, 0);

    gConnectingLabel = lv_label_create(scr);
    lv_obj_set_width(gConnectingLabel, 190);
    lv_label_set_long_mode(gConnectingLabel, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(gConnectingLabel, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(gConnectingLabel, LV_ALIGN_RIGHT_MID, -12, 0);

    // The QR is spent, so nothing needs the reduced backlight any more.
    displaySetBrightness(kBrightnessNormal);
  }

  lv_label_set_text(gConnectingLabel, status.c_str());
  Serial.println("[Connecting] " + status);
  displayTick();
}

void showStatusScreen(const String &wifiLine, const String &mqttLine, bool wifiConnected) {
  lv_obj_t *scr = makeBlankScreen();

  // Icon in the same left slot the connecting spinner occupies, so the
  // screen reads as that one resolving rather than as a different screen.
  //
  // LV_SYMBOL_* are glyphs in the built-in Montserrat font, not images —
  // which is what keeps this out of the Icons8 pipeline entirely. That
  // pipeline exists for menu-node artwork (ARCHITECTURE.md §5/§12); status
  // chrome like this is what LVGL's own symbol set is for, and it costs no
  // download, no manifest entry, and no attribution.
  lv_obj_t *icon = lv_label_create(scr);
  lv_label_set_text(icon, wifiConnected ? LV_SYMBOL_WIFI : LV_SYMBOL_WARNING);
  lv_obj_set_style_text_font(icon, &lv_font_montserrat_28, 0);
  lv_obj_set_style_text_color(
      icon, lv_palette_main(wifiConnected ? LV_PALETTE_GREEN : LV_PALETTE_RED), 0);
  lv_obj_align(icon, LV_ALIGN_LEFT_MID, 26, 0);

  lv_obj_t *label = lv_label_create(scr);
  String text = wifiLine + "\n\n" + mqttLine + "\n\nPress any button to continue";
  lv_label_set_text(label, text.c_str());
  lv_obj_set_width(label, 224);
  lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
  lv_obj_align(label, LV_ALIGN_RIGHT_MID, -8, 0);

  Serial.println("[Status] " + wifiLine);
  Serial.println("[Status] " + mqttLine);

  // "Any button press" per docs/POC.md §2 step 7 — deliberately not
  // filtered to one event type, unlike everywhere else in the real input
  // scheme.
  waitForInput(/*acceptAny=*/true);
}

} // namespace

void setup() {
  Serial.begin(115200);
  delay(2000);
  Serial.println("=== ESP32-C6 Remote Control Panel POC ===");
  Serial.println("[Serial] type 'r' + Enter at any time to clear provisioning "
                  "and restart (re-shows the pairing QR)");

  littleFsSmokeTest();

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
}

void loop() {
  showWelcomeScreen();
  pumpFor(2000);
  addIconToWelcomeScreen();
  pumpFor(2000);

  showDeviceInfoScreen();
  showAboutScreen();

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
      showStatusScreen("Wi-Fi: provisioning failed or timed out", "MQTT: not attempted",
                       /*wifiConnected=*/false);
      return; // loop() re-enters from the top
    }
  }

  // Credentials come from our own NVS copy, NOT from WiFi.SSID()/psk():
  // those report the currently associated AP, which on a fresh boot is
  // nothing at all (see provisioningLoadStoredWifi).
  const WifiCredentials wifiCreds = provisioningLoadStoredWifi();

  // Same spinner on the already-provisioned path: this leg also blocks for
  // ~15s per attempt, and without it the previous screen just sits there
  // looking frozen.
  showConnectingScreen("Connecting to Wi-Fi\n\n" + wifiCreds.ssid);
  WifiResult wifi =
      netConnectWifi(wifiCreds.ssid, wifiCreds.password, 15000, []() { displayTick(); });
  String wifiLine = wifi.connected
                        ? ("Wi-Fi: " + wifiCreds.ssid + " — Connected (" + wifi.ip + ")")
                        : ("Wi-Fi: Failed (" + wifi.errorReason + ")");

  String mqttLine;
  if (wifi.connected && mqttCfg.valid) {
    showConnectingScreen("Connecting to MQTT\n\n" + mqttCfg.brokerHost);
    MqttResult mqtt = netConnectMqttAndTestEcho(
        mqttCfg.brokerHost, mqttCfg.brokerPort, mqttCfg.useTls, mqttCfg.clientId,
        mqttCfg.username, mqttCfg.password, 15000, []() { displayTick(); });
    if (mqtt.connected && mqtt.echoOk) {
      mqttLine = "MQTT: Connected";
    } else if (mqtt.connected) {
      mqttLine = "MQTT: Connected, no echo (" + mqtt.errorReason + ")";
    } else {
      mqttLine = "MQTT: Failed (" + mqtt.errorReason + ")";
    }
  } else {
    mqttLine = "MQTT: not attempted (Wi-Fi unavailable)";
  }

  showStatusScreen(wifiLine, mqttLine, wifi.connected);
  // loop() re-enters from the top — docs/POC.md §2 step 8
}
