#include "device_info.h"

#include <WiFi.h>
#include <esp_chip_info.h>
#include <esp_flash.h>
#include <lvgl.h>

std::vector<String> collectDeviceInfoLines() {
  std::vector<String> lines;

  esp_chip_info_t chip;
  esp_chip_info(&chip);
  lines.push_back("Chip: ESP32-" + String(chip.model == CHIP_ESP32C6 ? "C6" : "?") +
                   " rev " + String(chip.revision));
  lines.push_back("Cores: " + String(chip.cores));
  lines.push_back("CPU: " + String(getCpuFrequencyMhz()) + " MHz");

  uint32_t flashSize = 0;
  esp_flash_get_size(nullptr, &flashSize);
  lines.push_back("Flash: " + String(flashSize / (1024 * 1024)) + " MB");

  lines.push_back("MAC: " + WiFi.macAddress());

  lines.push_back("Free heap: " + String(ESP.getFreeHeap() / 1024) + " KB");
  lines.push_back("Min free heap: " + String(ESP.getMinFreeHeap() / 1024) + " KB");
#if CONFIG_SPIRAM
  lines.push_back("Free PSRAM: " + String(ESP.getFreePsram() / 1024) + " KB");
#else
  lines.push_back("PSRAM: not present/enabled");
#endif

  lines.push_back(String("LVGL: v") + lv_version_major() + "." + lv_version_minor() + "." +
                   lv_version_patch());

  lines.push_back("Built: " __DATE__ " " __TIME__);

  lines.push_back("Uptime: " + String(millis() / 1000) + "s");

  return lines;
}
