// Device diagnostics over MQTT, for the web console:
//
//   telemetry  devices/<id>/telemetry, every 30 s while online (every 2 s
//              for a while after a "telemetry" command, while the device
//              page is open): uptime, CPU load, memory, Wi-Fi, battery, chip
//              temperature.
//   log        devices/<id>/log, only while the console's Logs page asks
//              ("log_stream" command, renewed every minute): the serial log,
//              starting with what's still in the ring buffer.
//   reboot     "reboot" command: restarts the panel.
//   shell      "shell" command with a text line: help, stats, tasks, wifi,
//              reboot, or input keys. Output is printed, so it comes back
//              through the log stream.
#pragma once
#include <ArduinoJson.h>

// Call from the main loop (appPump).
void diagService();

// Commands from the Core: "telemetry", "log_stream", "reboot", "shell". True if
// `action` was one of them.
bool diagHandleCommand(const String &action, JsonVariantConst doc);
