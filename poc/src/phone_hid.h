#pragma once

#include <Arduino.h>

// The panel as a Bluetooth LE media remote for the user's phone: a HID
// "consumer control" device (like a keyboard's volume keys), which Android
// and iOS both accept without an app. The ESP32-C6 has no Bluetooth Classic,
// so AVRCP (what headphones use) is not available — HID over GATT is the
// BLE way to send volume up/down.
//
// Pairing: once, from the phone's Bluetooth settings ("Just Works", no PIN);
// the bond is kept in NVS and the phone reconnects on its own after that.
//
// Volume via HID is relative only: a phone never reports its level back, so
// anything shown as a level on the panel is an estimate.

// Starts BLE and advertises as `name`. Call once, after provisioning is done
// (provisioning owns BLE while it runs). Safe to call again (no-op).
void phoneHidBegin(const char *name);

bool phoneHidConnected();

// Queues `steps` volume presses: >0 up, <0 down. Sent one press/release at a
// time from phoneHidPoll(), so a fast slider drag doesn't flood the link.
void phoneHidVolumeSteps(int steps);

// Call from the main loop (appPump does): sends queued presses.
void phoneHidPoll();

// Name the phone sees in its Bluetooth list.
const char *phoneHidName();
