#include "phone_hid.h"

#include <BLEDevice.h>
#include <BLEHIDDevice.h>
#include <BLESecurity.h>
#include <BLEServer.h>

namespace {

// One input report (ID 1), one byte: bit0 Volume Increment, bit1 Volume
// Decrement, bit2 Mute, rest padding. Consumer page usages per HID Usage
// Tables §15.
const uint8_t kReportMap[] = {
    0x05, 0x0C,  // Usage Page (Consumer)
    0x09, 0x01,  // Usage (Consumer Control)
    0xA1, 0x01,  // Collection (Application)
    0x85, 0x01,  //   Report ID (1)
    0x15, 0x00,  //   Logical Minimum (0)
    0x25, 0x01,  //   Logical Maximum (1)
    0x75, 0x01,  //   Report Size (1)
    0x95, 0x03,  //   Report Count (3)
    0x09, 0xE9,  //   Usage (Volume Increment)
    0x09, 0xEA,  //   Usage (Volume Decrement)
    0x09, 0xE2,  //   Usage (Mute)
    0x81, 0x02,  //   Input (Data, Variable, Absolute)
    0x95, 0x05,  //   Report Count (5)
    0x81, 0x03,  //   Input (Constant) — padding
    0xC0,        // End Collection
};

constexpr uint8_t kVolUp = 0x01;
constexpr uint8_t kVolDown = 0x02;
// A press must be visible to the phone as its own report pair; ~30ms between
// edges is what keyboards do and phones register every one.
constexpr uint32_t kEdgeMs = 30;

bool gStarted = false;
volatile bool gConnected = false;
BLECharacteristic *gInput = nullptr;
String gName;

int gPending = 0;          // queued steps, sign = direction
bool gKeyDown = false;     // a press is out, release due
uint32_t gLastEdge = 0;

class ServerCb : public BLEServerCallbacks {
  void onConnect(BLEServer *) override {
    gConnected = true;
    Serial.println("[PhoneHID] phone connected");
  }
  void onDisconnect(BLEServer *) override {
    gConnected = false;
    gPending = 0;
    gKeyDown = false;
    Serial.println("[PhoneHID] phone disconnected — advertising again");
    BLEDevice::startAdvertising();
  }
};

class SecurityCb : public BLESecurityCallbacks {
  bool onSecurityRequest() override { return true; }
  uint32_t onPassKeyRequest() override { return 0; }
  void onPassKeyNotify(uint32_t) override {}
  bool onConfirmPIN(uint32_t) override { return true; }
#if defined(CONFIG_NIMBLE_ENABLED)
  void onAuthenticationComplete(ble_gap_conn_desc *desc) override {
    Serial.printf("[PhoneHID] pairing %s\n", desc->sec_state.encrypted ? "ok (bonded)" : "FAILED");
  }
#else
  void onAuthenticationComplete(esp_ble_auth_cmpl_t r) override {
    Serial.printf("[PhoneHID] pairing %s\n", r.success ? "ok (bonded)" : "FAILED");
  }
#endif
};

void sendReport(uint8_t bits) {
  if (!gInput) return;
  gInput->setValue(&bits, 1);
  gInput->notify();
}

} // namespace

void phoneHidBegin(const char *name) {
  if (gStarted) return;
  gStarted = true;
  gName = name;

  BLEDevice::init(gName.c_str());
  BLESecurity *sec = new BLESecurity();
  sec->setCapability(ESP_IO_CAP_NONE);          // Just Works: no PIN on a panel
  sec->setAuthenticationMode(true, false, true);  // bonding, no MITM, secure connections
  BLEDevice::setSecurityCallbacks(new SecurityCb());

  BLEServer *server = BLEDevice::createServer();
  server->setCallbacks(new ServerCb());

  BLEHIDDevice *hid = new BLEHIDDevice(server);
  hid->manufacturer()->setValue("nodrige");
  hid->pnp(0x02, 0xE502, 0xA111, 0x0100);
  hid->hidInfo(0x00, 0x01);
  hid->reportMap(const_cast<uint8_t *>(kReportMap), sizeof(kReportMap));
  gInput = hid->inputReport(1);
  hid->setBatteryLevel(100);
  hid->startServices();

  BLEAdvertising *adv = BLEDevice::getAdvertising();
  adv->setAppearance(GENERIC_HID);
  adv->addServiceUUID(hid->hidService()->getUUID());
  adv->setScanResponse(true);
  BLEDevice::startAdvertising();
  Serial.printf("[PhoneHID] advertising as '%s' — pair it from the phone's Bluetooth settings\n",
                gName.c_str());
}

bool phoneHidConnected() { return gConnected; }

const char *phoneHidName() { return gName.c_str(); }

void phoneHidVolumeSteps(int steps) {
  if (!gConnected) return;
  gPending += steps;
}

void phoneHidPoll() {
  if (!gConnected || millis() - gLastEdge < kEdgeMs) return;
  if (gKeyDown) {
    sendReport(0);
    gKeyDown = false;
    gLastEdge = millis();
  } else if (gPending != 0) {
    sendReport(gPending > 0 ? kVolUp : kVolDown);
    gPending += gPending > 0 ? -1 : 1;
    gKeyDown = true;
    gLastEdge = millis();
  }
}
