#include "settings.h"

#include <Preferences.h>

namespace {
constexpr const char *kNamespace = "settings";
constexpr const char *kDimKey = "dimSec";
constexpr const char *kOffKey = "offSec";
constexpr const char *kVolTargetKey = "volTarget";
constexpr const char *kSwitchSyncKey = "swSyncSec";

uint32_t getU(const char *key, uint32_t def) {
  Preferences p;
  p.begin(kNamespace, true);
  const uint32_t v = p.getUInt(key, def);
  p.end();
  return v;
}

void putU(const char *key, uint32_t v) {
  Preferences p;
  p.begin(kNamespace, false);
  p.putUInt(key, v);
  p.end();
}
} // namespace

uint32_t settingsDimAfterSec() { return getU(kDimKey, kDefaultDimAfterSec); }
void settingsSetDimAfterSec(uint32_t seconds) { putU(kDimKey, seconds); }
uint32_t settingsOffAfterSec() { return getU(kOffKey, kDefaultOffAfterSec); }
void settingsSetOffAfterSec(uint32_t seconds) { putU(kOffKey, seconds); }
uint8_t settingsVolumeTarget() { return getU(kVolTargetKey, kVolumeTargetPhone) == kVolumeTargetComputer ? kVolumeTargetComputer : kVolumeTargetPhone; }
void settingsSetVolumeTarget(uint8_t target) { putU(kVolTargetKey, target); }
uint32_t settingsSwitchSyncSec() { return getU(kSwitchSyncKey, kDefaultSwitchSyncSec); }
void settingsSetSwitchSyncSec(uint32_t seconds) { putU(kSwitchSyncKey, seconds); }
