#include "diag.h"

#include <WiFi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "app.h"
#include "net.h"
#include "ui_layout.h"

// --- CPU load ------------------------------------------------------------------------
//
// FreeRTOS run-time stats (enabled in this core's sdkconfig) count how long
// each task has run. The C6 has one core, so load = 1 - the idle task's
// share of the time since the last sample.

namespace {

uint32_t gPrevTotal = 0, gPrevIdle = 0;
float gCpu = -1;

void sampleCpu() {
  const UBaseType_t n = uxTaskGetNumberOfTasks();
  TaskStatus_t *tasks = (TaskStatus_t *)malloc(sizeof(TaskStatus_t) * (n + 2));
  if (!tasks) return;
  configRUN_TIME_COUNTER_TYPE total = 0;
  const UBaseType_t got = uxTaskGetSystemState(tasks, n + 2, &total);
  uint32_t idle = 0;
  for (UBaseType_t i = 0; i < got; i++) {
    if (strncmp(tasks[i].pcTaskName, "IDLE", 4) == 0) idle += tasks[i].ulRunTimeCounter;
  }
  free(tasks);
  const uint32_t dTotal = (uint32_t)total - gPrevTotal, dIdle = idle - gPrevIdle;
  if (gPrevTotal && dTotal) {
    float load = 100.0f * (1.0f - (float)dIdle / (float)dTotal);
    gCpu = load < 0 ? 0 : (load > 100 ? 100 : load);
  }
  gPrevTotal = (uint32_t)total;
  gPrevIdle = idle;
}

void printTasks() {
  const UBaseType_t n = uxTaskGetNumberOfTasks();
  TaskStatus_t *tasks = (TaskStatus_t *)malloc(sizeof(TaskStatus_t) * (n + 2));
  if (!tasks) return;
  configRUN_TIME_COUNTER_TYPE total = 0;
  const UBaseType_t got = uxTaskGetSystemState(tasks, n + 2, &total);
  Serial.printf("%-16s %6s %5s %8s\n", "task", "cpu%", "prio", "stack");
  for (UBaseType_t i = 0; i < got; i++) {
    const TaskStatus_t &t = tasks[i];
    Serial.printf("%-16s %6.1f %5u %8u\n", t.pcTaskName, total ? 100.0 * t.ulRunTimeCounter / total : 0.0,
                  (unsigned)t.uxCurrentPriority, (unsigned)t.usStackHighWaterMark);
  }
  Serial.println("(cpu% since boot; stack = least free, in bytes)");
  free(tasks);
}

// --- Telemetry -------------------------------------------------------------------------

constexpr uint32_t kSlowMs = 30000, kFastMs = 2000;
uint32_t gLastTelemetry = 0, gFastUntil = 0;
bool gSendNow = true;

int batteryMv() {
  const uint32_t mv = analogReadMilliVolts(0) * 3;  // 200K/100K divider (menu.cpp batteryText)
  return mv < 2500 ? 0 : (int)mv;
}

void sendTelemetry() {
  sampleCpu();
  JsonDocument d;
  d["up"] = millis() / 1000;
  if (gCpu >= 0) d["cpu"] = roundf(gCpu * 10) / 10;
  JsonObject heap = d["heap"].to<JsonObject>();
  heap["free"] = ESP.getFreeHeap();
  heap["min"] = ESP.getMinFreeHeap();
  heap["size"] = ESP.getHeapSize();
  heap["largest"] = ESP.getMaxAllocHeap();
  d["mhz"] = getCpuFrequencyMhz();
  if (WiFi.isConnected()) d["rssi"] = WiFi.RSSI();
  const int mv = batteryMv();
  if (mv) d["batt"] = mv;
  d["temp"] = roundf(temperatureRead() * 10) / 10;
  d["tasks"] = uxTaskGetNumberOfTasks();
  d["screen"] = uiScreenName();
  char buf[384];
  const size_t len = serializeJson(d, buf, sizeof buf);
  netMqttPublishDiag(DiagTopic::Telemetry, buf, len);
}

// --- Log stream ---------------------------------------------------------------------

constexpr uint32_t kLogEveryMs = 300;
constexpr size_t kLogChunk = 1024;
uint32_t gLogUntil = 0;     // millis() when the console's lease runs out (0 = off)
uint32_t gLogCursor = 0;    // next byte of the log to send
uint32_t gLastLog = 0;
uint32_t gLogSeq = 0;

void sendLog() {
  static char chunk[kLogChunk + 1];
  for (int round = 0; round < 4; round++) {  // up to 4 KB per call when catching up
    uint32_t lost = 0;
    const size_t n = gLogTee.readSince(&gLogCursor, chunk, kLogChunk, &lost);
    if (!n && !lost) return;
    chunk[n] = 0;
    JsonDocument d;
    d["seq"] = gLogSeq++;
    if (lost) d["lost"] = lost;
    d["text"] = (const char *)chunk;
    static char out[kLogChunk * 2 + 96];
    const size_t len = serializeJson(d, out, sizeof out);
    if (!netMqttPublishDiag(DiagTopic::Log, out, len)) return;
    if (n < kLogChunk) return;
  }
}

// --- Shell -------------------------------------------------------------------------------

void printStats() {
  sampleCpu();
  const uint32_t s = millis() / 1000;
  Serial.printf("up      %lud %02luh %02lum %02lus\n", s / 86400, s / 3600 % 24, s / 60 % 60, s % 60);
  if (gCpu >= 0) Serial.printf("cpu     %.1f%% @ %lu MHz\n", gCpu, (unsigned long)getCpuFrequencyMhz());
  Serial.printf("heap    %u KB free of %u KB, low %u KB, largest block %u KB\n", ESP.getFreeHeap() / 1024,
                ESP.getHeapSize() / 1024, ESP.getMinFreeHeap() / 1024, ESP.getMaxAllocHeap() / 1024);
  Serial.printf("chip    %s rev %d, %.1f C\n", ESP.getChipModel(), ESP.getChipRevision(), temperatureRead());
  const int mv = batteryMv();
  Serial.printf("battery %s\n", mv ? (String(mv / 1000.0f, 2) + " V").c_str() : "none");
  Serial.printf("screen  %s\n", uiScreenName());
}

void runShell(String line) {
  line.trim();
  Serial.println("$ " + line);
  const int sp = line.indexOf(' ');
  const String cmd = sp < 0 ? line : line.substring(0, sp);
  const String arg = sp < 0 ? String("") : line.substring(sp + 1);
  if (cmd == "help" || cmd == "?") {
    Serial.println("help              this list");
    Serial.println("stats             uptime, cpu, memory, battery");
    Serial.println("tasks             FreeRTOS tasks: cpu share, priority, free stack");
    Serial.println("wifi              Wi-Fi signal and address");
    Serial.println("keys <chars>      input keys: + - s b a h H v V k p t");
    Serial.println("reboot            restart the panel");
  } else if (cmd == "stats") {
    printStats();
  } else if (cmd == "tasks") {
    printTasks();
  } else if (cmd == "wifi") {
    if (!WiFi.isConnected()) {
      Serial.println("wifi    not connected");
    } else {
      Serial.printf("wifi    %d dBm, channel %d\n", WiFi.RSSI(), WiFi.channel());
      Serial.printf("ip      %s  gateway %s\n", WiFi.localIP().toString().c_str(), WiFi.gatewayIP().toString().c_str());
      Serial.printf("mac     %s\n", WiFi.macAddress().c_str());
    }
  } else if (cmd == "keys") {
    // The serial console's single-key commands, minus the ones that change
    // provisioning or block the loop (r, w).
    for (char c : arg) {
      if (c == 'r' || c == 'R' || c == 'w' || c == 'W') {
        Serial.printf("'%c' is serial-only\n", c);
        continue;
      }
      appSerialKey(c);
    }
  } else if (cmd == "reboot") {
    Serial.println("Restarting...");
    sendLog();  // let the console see it
    delay(300);
    ESP.restart();
  } else if (cmd.length()) {
    Serial.println(cmd + ": unknown command (try help)");
  }
}

}  // namespace

void diagService() {
  if (!netMqttIsConnected()) return;
  const uint32_t now = millis();
  const uint32_t every = (gFastUntil && (int32_t)(gFastUntil - now) > 0) ? kFastMs : kSlowMs;
  if (gSendNow || now - gLastTelemetry >= every) {
    gSendNow = false;
    gLastTelemetry = now;
    sendTelemetry();
  }
  if (gLogUntil) {
    if ((int32_t)(gLogUntil - now) <= 0) {
      gLogUntil = 0;
    } else if (now - gLastLog >= kLogEveryMs) {
      gLastLog = now;
      sendLog();
    }
  }
}

bool diagHandleCommand(const String &action, JsonVariantConst doc) {
  if (action == "telemetry") {
    // { "seconds": 120 }: fast updates for a while (the device page is open).
    const uint32_t secs = doc["seconds"] | 120;
    gFastUntil = millis() + (secs > 600 ? 600 : secs) * 1000;
    gSendNow = true;
    return true;
  }
  if (action == "log_stream") {
    // { "seconds": 120 } starts or renews; { "seconds": 0 } stops. A new
    // stream starts with the backlog still in the ring buffer.
    const uint32_t secs = doc["seconds"] | 120;
    if (!secs) {
      gLogUntil = 0;
      return true;
    }
    if (!gLogUntil || doc["backlog"] | false) {
      const uint32_t w = gLogTee.written();
      gLogCursor = w > LogTee::kRing ? w - LogTee::kRing : 0;
    }
    gLogUntil = millis() + (secs > 600 ? 600 : secs) * 1000;
    if (!gLogUntil) gLogUntil = 1;
    return true;
  }
  if (action == "shell") {
    runShell(String(doc["line"] | ""));
    return true;
  }
  return false;
}
