#pragma once

#include <Arduino.h>

// Services main.cpp provides to screen modules (menu.cpp). Kept tiny on
// purpose: screens drive their own loops, but every iteration must keep the
// display, serial console and online traffic alive.

// One iteration of background work: LVGL, serial commands, and — while
// online — incoming commands and config pushes. Call
// in every screen loop, as often as the loop runs.
void appPump();

// True if a task_result command for `requestId` has arrived since it was
// sent; `resultJson` receives that command's JSON. Consumes it.
bool appTakeTaskResult(const String &requestId, String &resultJson);

// Host of the MQTT broker this boot connected to ("" if none).
const String &appBrokerHost();

// One serial-console key command (+ - s b a h H v V k p t w r ...), as if
// typed on the USB serial console. The remote shell uses it (diag.cpp).
void appSerialKey(char c);
