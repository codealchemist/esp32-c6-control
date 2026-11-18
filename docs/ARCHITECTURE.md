# ESP32-C6 Remote Control Panel — Architecture Plan

## 1. Goal

A battery/wall-powered ESP32-C6 device with a 172×320 px display acts as a
physical remote control for a local backend ("the server"), communicating
over a local **MQTT broker** rather than HTTP. The device:

- Authenticates to the MQTT broker (connection-level credentials/TLS — see
  §8).
- Presents a configurable menu of **tasks** (direct actions, e.g. "Lights
  On") and **folders** (nested menus, e.g. "Messages") as a two-icon-visible
  horizontal carousel.
- Sends the selected task as an **MQTT publish**, optionally with a
  parameter (volume, brightness, …) that can be dialed in with a rotary
  encoder before sending.
- Receives push messages from the server (text + optional icon) via an
  **MQTT subscription**, queues them, and displays them in a scrollable
  text view.
- Is driven by a single physical button (short press = Back, long press =
  Home) plus a **KY-040** rotary encoder whose rotation is the main
  navigation/adjustment control and whose integrated push-button is
  select/confirm (§2, §6.2).
- Is fully described by a JSON document: menu tree, tasks, default params,
  whether a param is encoder-adjustable, icons, and list bindings. A default
  config is flashed with the firmware; the server can push a replacement at
  runtime.
- On first boot (or after a factory reset) has no Wi-Fi or MQTT broker
  details yet, and acquires them via a **BLE provisioning** flow (§11).

## 2. Hardware assumptions (confirm before locking HAL)

| Item    | Assumption                                                                 | Notes                                                                                                     |
| ------- | -------------------------------------------------------------------------- | --------------------------------------------------------------------------------------------------------- |
| MCU     | ESP32-C6 (single-core RISC-V, Wi-Fi 6, no PSRAM by default on most boards) | Confirm PSRAM presence — affects LVGL buffer sizing                                                       |
| Display | 172×320, SPI, **ST7789** (confirmed) — Waveshare ESP32-C6-LCD-1.47 | Confirmed from Waveshare's own example firmware (`Display_ST7789.cpp/.h` in [VolosR/waveshareBoards](https://github.com/VolosR/waveshareBoards/tree/main/C6example)). SPI pins from that same source: MISO=GPIO5, MOSI=GPIO6, SCLK=GPIO7, CS=GPIO14, DC=GPIO15, RST=GPIO21, Backlight=GPIO22, up to 80MHz — goes straight into `hal/board_pins.h` |
| Input   | 1 push-button (Back/Home) + 1 **KY-040** incremental rotary encoder w/ integrated push-button (confirmed) | KY-040 needs 2 GPIOs (CLK/DT quadrature) + 1 for its push-button, plus 1 for the dedicated button — 4 GPIOs total; mechanical contacts need debouncing (`input/encoder_driver.cpp`, `input/button_driver.cpp`). Exact pin numbers TBD in `hal/board_pins.h` |
| Storage | Onboard flash only, LittleFS partition                                     | No SD card assumed                                                                                        |
| Power   | Wall/USB powered, always-on Wi-Fi                                          | If battery-powered later, add a power-management module — out of scope for v1                             |

These are isolated behind a HAL layer (§6.1) specifically so a wrong
assumption is a one-file fix, not a rearchitecture.

## 3. Technology stack

| Layer          | Choice                                                                                                                        | Why                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                       |
| -------------- | ----------------------------------------------------------------------------------------------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Build system   | **PlatformIO**, Arduino framework for ESP32 (`espressif32` platform)                                                          | Fastest path to a working display + network stack on C6; ESP-IDF-native is more work for no benefit here since we're not fighting RTOS internals directly                                                                                                                                                                                                                                                                                                                                                                                                                                                                                 |
| UI framework   | **LVGL v9**                                                                                                                   | Only realistic choice for icon carousel + lists + scrolling text + rotary-encoder input groups on a device this small. LVGL's `indev` model natively supports encoder-as-navigation, which maps directly onto our nav/param/scroll modes                                                                                                                                                                                                                                                                                                                                                                                                  |
| Display driver | **LovyanGFX** (preferred over TFT_eSPI for cleaner multi-panel config and rotation handling) as the LVGL flush backend        | Swappable at the HAL boundary if the panel controller assumption is wrong                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                 |
| Networking     | Arduino `WiFi.h` for Wi-Fi; **ESP-IDF's `esp_mqtt_client`** (the `mqtt` component) for a single bidirectional MQTT connection | Replaces both the outbound REST client and the inbound async HTTP server with one connection: built-in TLS (mqtts), QoS 0/1/2, automatic reconnect with backoff, and Last Will and Testament (LWT) for online/offline presence — more robust and lower-overhead than polling/REST for this push-and-command pattern. Available from Arduino sketches on PlatformIO's `espressif32` platform because arduino-esp32 bundles the full ESP-IDF component tree. (Pure-Arduino fallback: `PubSubClient` — simpler API, but blocking, small fixed message buffer, and clunkier TLS setup; not recommended if "robust and performant" is the bar) |
| Config format  | JSON via **ArduinoJson v7**                                                                                                   | Streaming-friendly, low RAM overhead, well-suited to the fixed config tree                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                |
| Storage        | **LittleFS** for config/assets/icons, **NVS (Preferences)** for secrets (auth token, Wi-Fi creds)                             | Standard split: bulk/replaceable data in FS, small sensitive data in NVS                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                  |
| Icons          | LVGL C-array images baked in for defaults (sourced from Icons8, converted at build time — §12); **PNG** files on LittleFS, decoded via LVGL's built-in PNG decoder (`LV_USE_LODEPNG`), for server-pushed icons             | Baking defaults avoids a runtime image decoder for the common case. Pushed icons are PNG specifically because that's the format Icons8's free downloads come in (§12.2) — the backend can attach any Icons8 PNG straight to a message/config push without running its own image-conversion pipeline, which only the firmware build needs (§12)                                                                                                                                                                                                                                                                                                                                                                                                                                            |
| Provisioning   | ESP-IDF's `wifi_provisioning` component (built on `protocomm`) over **BLE**                                                   | Standard, battle-tested first-time-setup path for ESP32 devices; carries Wi-Fi credentials over its built-in endpoint and MQTT broker config over a custom endpoint we register alongside it (§11). Chosen over a SoftAP+HTTP captive portal specifically to avoid ever running an HTTP server on-device, matching the MQTT-only stance on steady-state networking                                                                                                                                                                                                                                                                        |

Display and input hardware are now both confirmed — nothing left in this
table is still an open assumption.

## 4. High-level module map

```
┌─────────────────────────────────────────────────────────────┐
│                            app/                              │
│   AppController — top-level state machine, owns everything   │
└───────┬───────────────┬───────────────┬───────────────┬─────┘
        │               │               │               │
   ┌────▼────┐    ┌─────▼─────┐   ┌─────▼─────┐   ┌─────▼─────┐
   │  ui/    │    │  input/   │   │   net/    │   │  config/  │
   │ LVGL    │    │ buttons + │   │ MQTT      │   │ load/     │
   │ screens │    │ encoder,  │   │ pub/sub,  │   │ validate/ │
   │         │    │ mode FSM  │   │ Wi-Fi     │   │ apply     │
   └────┬────┘    └─────┬─────┘   └─────┬─────┘   └─────┬─────┘
        │               │               │               │
        └───────────────┴───────┬───────┴───────────────┘
                                 │
                          ┌──────▼──────┐
                          │  util/      │
                          │ event_bus,  │
                          │ logger      │
                          └─────────────┘
                    (storage/ and hal/ underlie all of the above)
```

Modules talk to each other through a small **event bus** (pub/sub over a
FreeRTOS queue), not direct calls, so e.g. `net/` doesn't need to know about
`ui/` — it publishes `MessageReceivedEvent` and `ui/` (or `app/`) subscribes.
This keeps the UI, input, and network free-running tasks loosely coupled,
which matters because LVGL, the MQTT client's own task, and Wi-Fi all want
their own timing. Note this internal event bus is distinct from MQTT itself:
MQTT is the transport between device and server; the event bus is purely
in-process, decoupling `net/` from `ui/`/`app/`.

A fifth module, `provisioning/`, isn't in the steady-state diagram above
because it's only alive before the device has credentials (or during a
deliberate re-provisioning): `AppController` checks a "provisioned?" flag
at boot and either starts `net/`+`config/` normally, or hands off to
`provisioning/` (with `ui/` showing a QR-code screen) until it succeeds,
then reboots into the normal path. See §11.

## 5. Config document (the single source of truth)

### 5.1 Shape

`data/default_config.json` ships with the `menu` tree fully populated (that
part is genuinely a sane default) but with `mqtt.brokerHost`/`username`/
`topics` either absent or placeholder values — those are per-installation
and get filled in by the provisioning flow (§11), not baked into firmware.
The example below shows a _populated_ `mqtt` block as it would look after
provisioning, i.e. the shape of `/config/active.json` at runtime, not of
the burned-in default.

```jsonc
{
  "configVersion": 1,
  "mqtt": {
    "brokerHost": "192.168.1.50",
    "brokerPort": 8883,
    "useTls": true,
    "clientId": "esp32-panel-01",
    "username": "esp32-panel-01",
    "topics": {
      "commands": "devices/esp32-panel-01/cmd",
      "commandResults": "devices/esp32-panel-01/cmd/result",
      "messages": "devices/esp32-panel-01/messages",
      "config": "devices/esp32-panel-01/config",
      "status": "devices/esp32-panel-01/status"
    }
  },
  "menu": [
    {
      "id": "lights-on",
      "type": "task",
      "label": "Lights On",
      "icon": "icon_light_on",
      "task": {
        "payload": { "taskId": "lights-on", "state": "on" },
        "qos": 1,
        "params": []
      }
    },
    {
      "id": "player-volume",
      "type": "task",
      "label": "Volume",
      "icon": "icon_volume",
      "task": {
        "payload": { "taskId": "player-volume" },
        "qos": 1,
        "expectsResult": true,
        "params": [
          {
            "key": "level",
            "type": "int",
            "min": 0,
            "max": 100,
            "step": 1,
            "default": 40,
            "encoderAdjustable": true
          }
        ]
      }
    },
    {
      "id": "messages",
      "type": "folder",
      "label": "Messages",
      "icon": "icon_mail",
      "binding": "message_inbox",
      "children": []
    },
    {
      "id": "scenes",
      "type": "folder",
      "label": "Scenes",
      "icon": "icon_folder",
      "children": [
        {
          "id": "scene-movie",
          "type": "task",
          "label": "Movie",
          "icon": "icon_movie",
          "task": {
            "payload": { "taskId": "scene-movie", "name": "movie" },
            "qos": 1
          }
        }
      ]
    },
    {
      "id": "settings",
      "type": "folder",
      "label": "Settings",
      "icon": "icon_settings",
      "children": [
        {
          "id": "reprovision",
          "type": "action",
          "label": "Reset Wi-Fi/MQTT",
          "icon": "icon_reset",
          "action": "reprovision"
        },
        {
          "id": "restore-config",
          "type": "action",
          "label": "Restore Default Config",
          "icon": "icon_restore",
          "action": "restoreDefaultConfig"
        },
        {
          "id": "about",
          "type": "action",
          "label": "About",
          "icon": "icon_info",
          "action": "showAbout"
        }
      ]
    }
  ]
}
```

**Topic directions** (aligned with the nodrige Core, which is the server
end of every topic — revised from the original draft, which had `commands`
and `messages` the other way round):

| Topic            | Direction       | Carries                                                                 |
| ---------------- | --------------- | ----------------------------------------------------------------------- |
| `messages`       | device → server | Every device-originated event as a CoreEvent `{componentId, eventType, value, metadata}`: telemetry, and **task invocations** (`eventType: "task"`, §6.3) |
| `commands`       | server → device | Every server-originated command `{action, value, correlationId, ...}`: e.g. `echo`, and **task replies** (`action: "task_result"`, §6.3) |
| `commandResults` | device → server | Outcomes of what the server sent the device, e.g. config push results `{type: "config", status, errors}` (§5.3) |
| `config`         | server → device | Retained config documents (§5.3)                                        |
| `status`         | device → server | Presence/LWT (§8)                                                       |

Each direction is one topic per device rather than one per task, which keeps
broker-side authorization simple (§8): a device may publish only to
`messages`, `commandResults` and `status`, and subscribe only to `commands`
and `config`.

QoS per topic (confirmed):

| Topic            | QoS | Why                                                                 |
| ----------------- | --- | -------------------------------------------------------------------- |
| `messages`         | 1   | Carries task invocations: a dropped "lights on" is bad UX (§5.2)     |
| `commands`         | 1   | Carries task replies — a result worth publishing is worth delivering |
| `config`           | 1   | A dropped config push shouldn't silently fail to land                |
| `commandResults`   | 1   | A config outcome the server never sees leaves it guessing            |
| `status`           | 0   | Presence/LWT (§8) is inherently a "latest state wins" signal — the next publish supersedes a lost one, so at-most-once is fine |

The MQTT **password** (or TLS client cert/key, if using mutual TLS) is
provisioned to NVS separately, never stored in the JSON config alongside
`username`/`clientId` — same rationale as the old auth-token handling.

### 5.2 Node types

- **`task`** — leaf. Has a `payload` object (merged with any live param
  values and a generated `requestId` before publish), an optional MQTT
  `qos` (defaults to 1 — "at least once" — appropriate for commands where a
  dropped "lights on" is bad UX), and an optional `expectsResult` (defaults
  to `false`). The task's `params` array is the **sole source of truth for
  what inputs it accepts and how they're edited** — see §5.2.1. A param
  with `encoderAdjustable: true` gets an on-device edit step before the
  task is sent; `encoderAdjustable: false` (or omitted) means the param is
  always sent at its `default` and is only ever changed by pushing a new
  config (§5.3), never edited in the field. `expectsResult` is per-task
  because whether waiting for a correlated confirmation makes sense is a
  business decision, not a protocol one: `true` means the device
  correlates a reply on `cmd/result` by `requestId` and can act on its
  contents (§6.3); `false` (the default) means fire-and-forget — the
  device shows a local "sent" acknowledgment immediately and never waits
  or times out.
- **Offline.** Once paired, the panel always boots to the menu: one
  foreground Wi-Fi attempt (8 s) and up to 4 s for the broker, then Wi-Fi
  and MQTT keep reconnecting in the background. The home header shows
  Online / No server / No Wi-Fi. While the broker is unreachable, `task` and
  `switch` tiles are dimmed and a tap just shows the red cross (nothing is
  queued, §6.3); local actions, folders and the phone volume keep working.
  Everything lights up again on reconnect, with no reboot.
- **Shortcut button (`"shortcut"` field).** Marks a task or switch as one
  the physical shortcut button may run; other types are rejected, since a
  press can't choose a value (Volume, for example, is not a fit).
  - `"shortcut": true` on a switch: each press toggles it, like a tap.
  - `"shortcut": true` on a task: each press runs it.
  - `"shortcut": { "off": { "tuyaScene": "Turn Off" } }` on a task: presses
    alternate. While the task's scene is active (as the panel knows it: its
    last run, or the Core's `scene_state`, asked at boot and when a scenes
    page opens) the press runs the task with `off` merged into the payload;
    otherwise it runs the task normally.

  The press works from any menu page and also on a dimmed or off screen
  (it wakes it *and* runs); a pill at the bottom shows progress and the
  result.
- **Task feedback on the tile.** Every `task` runs from its menu tile: any
  adjustable params open their editor first, then the grid returns and a
  small spinner at the tile's top-right shows the reply is pending; it turns
  into a green check or red cross for 3 s (no reply in 5 s = cross).
  Fire-and-forget tasks show the check once sent. Message text goes to the
  log, not the screen.
- **Active scene.** Tasks whose payload names a `tuyaScene` get a lit tile
  while that scene is active. Tuya has no notion of a current scene, so the
  Core learns one: 10 s after a scene runs (cancelled if another scene runs
  first) it stores the states of the scene's lights as its fingerprint. When
  a page with scene tiles opens, the panel asks (a `scene-state` task) and
  the Core answers `{ action: "scene_state", active: [<lowercase names>] }`
  on the commands topic: the scene that ran in the last 30 s, otherwise the
  scenes whose fingerprints match the lights now. A successful scene run
  lights its tile immediately; nothing re-checks until the page is opened
  again (no polling, no pushes).
- **`switch`** — leaf with two states, on and off, shown as a toggle on its
  menu item (icon lit when on). Each tap flips it and publishes a task
  exactly like `task` does (§6.3), with `switch.payload` plus
  `state: "on" | "off"` plus that state's own optional object, so each state
  can carry different data:

  ```json
  { "id": "lights", "type": "switch", "label": "Lights", "icon": "icon_light_on",
    "switch": { "payload": { "taskId": "lights" }, "qos": 1, "expectsResult": true,
                "default": false,
                "on":  { "tuyaScene": "Turn On" },
                "off": { "tuyaScene": "Turn Off" } } }
  ```

  The state is kept in NVS across reboots, starting at `default`, and shown
  immediately on tap. With `expectsResult`, an error reply or no reply within
  5s flips it back and says why. To catch changes made elsewhere (the Smart
  Life app, a wall switch), the panel asks the Core for the real state
  (`switch-state` task with the `on` object's `tuyaScene`; the Core reports
  "on" if any light that scene acts on is on) at boot and on screen wake, at
  most once per the "Switch sync" setting (default 10 min; Never = boot only).
- **`folder`** — has `children`, another array of nodes, rendered as its own
  carousel level when entered. Supports arbitrary nesting depth.
- **`folder` with `binding`** — instead of static `children`, the folder's
  content is supplied by a runtime data source. `"message_inbox"` is the
  built-in binding for the received-messages list; this is the extension
  point if other dynamic lists are needed later (e.g. "recent tasks").
- **`action`** — leaf that triggers a local firmware behavior instead of
  publishing to MQTT, identified by a fixed `action` field. Used for
  on-device system functions exposed through the same menu system rather
  than a separate settings UI: `"reprovision"` (re-enter BLE provisioning,
  §11.4), `"restoreDefaultConfig"` (copy `default_config.json` back over
  `active.json`, §5.3 point 4), and `"showAbout"` (a static text screen —
  reuses `MessageScreen`'s rendering, not a new screen type — showing the
  Icons8 attribution required by the free license tier, §12.1), and
  `"volume"` (a slider with a Phone | Computer toggle: for the phone it sends
  volume up/down key presses to the user's phone, paired to the panel as a
  Bluetooth LE HID media remote — the C6 has no Bluetooth Classic, so HID over
  GATT rather than AVRCP, and the phone never reports its level, so that
  value is an estimate; for the computer it sends the Core's `player-volume`
  task, which sets and reports the real 0-100 output level of the machine
  the Core runs on. `"phoneVolume"` is accepted as an older name), and
  `"screenDim"` (how long without input before the backlight dims — Never,
  10 s … 10 min, default 30 s; an on-device setting kept in NVS, not in the
  config; the touch that wakes a dimmed screen does nothing else),
  `"shortcut"` (picks the menu item the physical shortcut button runs, from
  the items the config marks with a `shortcut` field — see below; kept in
  NVS by node id, so it survives config pushes as long as that id stays in
  the menu), and
  `"screenOff"` (same idea one step further: after Never / 30 s … 30 min,
  default 5 min, the backlight goes to 0 and the panel sleeps; any touch,
  button or encoder input turns it back on and is otherwise ignored).
  Handled by
  `app/system_actions.cpp/.h` (POC: `menu.cpp`, `phone_hid.cpp`).

#### 5.2.1 Parameter types

`ui/` never hardcodes per-task input handling — it renders and drives every
param purely from its `type`, which is what keeps `ParamEditScreen` a
single generic screen instead of one screen per task. The device has
exactly two physical controls: a **KY-040 rotary encoder** (rotate + its
own push-button) and **one dedicated button** (short press / long press).
Every type below is built from just those, following one consistent rule
carried over from §1/§2: **button short-press = cancel, button long-press =
Home** (jumps to the root menu, abandoning any in-progress edit) —
`string` entry is the sole, deliberate exception, noted below.

| `type`    | Schema fields                                   | Widget                                          | Encoder rotate                                      | Encoder push                                                          | Button                                    |
| --------- | ------------------------------------------------ | ------------------------------------------------ | ---------------------------------------------------- | ------------------------------------------------------------------------ | ------------------------------------------- |
| `int`     | `min`, `max`, `step`, `default`                   | Numeric stepper                                   | Value ± `step` (accelerates on fast rotation — larger steps the faster the encoder turns, so a 0–100 range doesn't take 100 detents) | Confirm + send                                                        | Short = cancel, Long = Home                 |
| `float`   | `min`, `max`, `step`, `decimals`, `default`       | Numeric stepper with fixed decimals               | Same, accelerated                                    | Confirm + send                                                        | Short = cancel, Long = Home                 |
| `options` | `choices: [{ value, label }]`, `default`, `wrap`  | Single-line picker showing the current choice     | Next/previous choice (wraps if `wrap:true`)          | Confirm + send                                                        | Short = cancel, Long = Home                 |
| `bool`    | `default`, `onLabel`/`offLabel` (optional)        | Switch                                             | Flip true/false                                      | Confirm + send                                                        | Short = cancel, Long = Home                 |
| `date`    | `min`, `max` (optional), `format` (e.g. `YYYY-MM-DD`) | Segmented fields (year / month / day), one highlighted at a time | ± 1 on the highlighted field, clamped/rolled over per field (month 1–12, day per days-in-month) | Short = move to next field (wraps year→month→day→year); Long = confirm whole date + send | Short = cancel, Long = Home                 |
| `time`    | `format` (`HH:mm` or `HH:mm:ss`), `step` (minute granularity), `default` | Segmented fields (hour / minute[/second])          | ± `step` on the highlighted field                    | Short = move to next field (wraps); Long = confirm + send             | Short = cancel, Long = Home                 |
| `string`  | `maxLength`, `charset` (`alnum` / `numeric` / `custom:<chars>`), `default` | Character-wheel text entry, always appending at the end (no arbitrary caret positioning — hardware doesn't support it) | Cycle the **next** character through `charset`       | Short = accept the current character and advance (extends the string); Long = commit the whole string + send | **Short = delete last character (backspace); Long = cancel whole edit** |

`string` is the one mode where the button's meaning is deliberately
overridden from the short=cancel/long=Home rule above, because free-text
entry genuinely needs a delete affordance and there's no other control
left to assign it to. The tradeoff: **Home isn't directly reachable while
in `PARAM_EDIT_TEXT`** — a long button-press there cancels the edit back to
`NAVIGATE` first, and Home is a second long-press from there. This was
judged an acceptable, rare-case cost for keeping the rest of the input
scheme uniform. `string` is also deliberately the most awkward of these —
a character-wheel with no keyboard is inherently slow — so **prefer
`options` over `string` wherever the set of valid values is enumerable**
(scene names, presets, input sources, …); `string` should be reserved for
genuinely free-form values (e.g. a custom label), not the default choice
for text-shaped data.

If a task has more than one `encoderAdjustable` param, `ParamEditScreen`
presents them one at a time in the order they appear in the `params` array;
confirming the last one sends the task with all collected values. A cancel
(button short-press, or long-press within `PARAM_EDIT_TEXT`) at *any* step
discards the entire sequence back to `NAVIGATE` — partial edits are never
partially sent (no task fires with only some of its params filled in).

Example param objects, one per type:

```jsonc
{ "key": "level", "type": "int", "min": 0, "max": 100, "step": 1, "default": 40, "encoderAdjustable": true }

{ "key": "targetTemp", "type": "float", "min": 16.0, "max": 28.0, "step": 0.5, "decimals": 1, "default": 21.0, "encoderAdjustable": true }

{ "key": "mode", "type": "options", "default": "eco", "wrap": true, "encoderAdjustable": true,
  "choices": [
    { "value": "off", "label": "Off" },
    { "value": "eco", "label": "Eco" },
    { "value": "boost", "label": "Boost" }
  ] }

{ "key": "enabled", "type": "bool", "default": true, "onLabel": "On", "offLabel": "Off", "encoderAdjustable": true }

{ "key": "startDate", "type": "date", "format": "YYYY-MM-DD", "default": "2026-08-11", "encoderAdjustable": true }

{ "key": "alarmTime", "type": "time", "format": "HH:mm", "step": 5, "default": "07:30", "encoderAdjustable": true }

{ "key": "label", "type": "string", "maxLength": 16, "charset": "alnum", "default": "", "encoderAdjustable": true }
```

### 5.3 Config lifecycle

1. **Build time**: `data/default_config.json` is part of the LittleFS image
   (`data/` → `pio run -t uploadfs`), so it ships burned-in with every
   firmware flash.
2. **Boot**: `config_manager` reads `/config/active.json` from LittleFS; if
   absent (first boot) or invalid, it copies `default_config.json` into that
   slot and loads it.
3. **Runtime update**: the server publishes a new config document to the
   device's subscribed `mqtt.topics.config` topic (retained, so a device
   that reconnects after being offline immediately receives the latest
   config on subscribe, not just future pushes). `config_manager`:
   - Validates JSON syntax and required fields (`configVersion`, `menu`
     array shape) before touching anything on disk.
   - Writes to `/config/staged.json` first, validates it loads cleanly into
     the menu model, then atomically renames it over `/config/active.json`.
   - **Reboots** to apply it (confirmed: always a full reboot, no live
     UI/menu reinitialization — simplest and safest, and a config push is
     not a latency-sensitive path).
4. **Recovery**: "Restore Default Config" is a menu entry (an `action` node,
   §5.2) that copies `default_config.json` back over `active.json` and
   reboots — reachable through Settings whenever the menu itself is still
   navigable. For the case where a bad config makes the menu unusable, the
   hardware factory-reset combo (§11.4) is the fallback, and also restores
   the default config as part of a full reset.
5. **Icons referenced by ID** (`"icon_light_on"`) resolve first against the
   baked-in LVGL image table (built from Icons8 source art at compile
   time — see §12), then against `/assets/icons/<id>.png` on LittleFS
   (PNG, decoded via LVGL's PNG decoder — §3) for ones delivered at
   runtime (e.g. alongside a message).

## 6. Runtime behavior

### 6.1 Screens (`ui/screens/`)

- **IconCarouselScreen** — renders exactly 2 icons at a time from the
  current menu level, with the selected one highlighted; encoder rotation
  shifts the visible window and selection (§6.2).
- **ListScreen** — used for `folder` nodes with many children beyond icon
  count, and for the message inbox; simple vertically scrollable list.
- **MessageScreen** — full-screen text view for a single message; long text
  scrolls, driven by the encoder.
- **ParamEditScreen** — a single generic screen shown for any
  `encoderAdjustable` param; it picks its layout and behavior from the
  param's `type` (§5.2.1) rather than being one screen per task. Steps
  through a task's params one at a time when there's more than one.

### 6.2 Input mode state machine (`input/input_manager`)

Rather than hardcode "what does turning the encoder do" per screen, input
dispatch is mediated by an explicit mode enum, set by `app/AppController`
whenever it changes screen. Every mode is built from exactly two physical
controls — the KY-040's rotate + push, and one button with short/long
press — confirmed in §1/§2:

```
NAVIGATE             — encoder rotate moves icon selection left/right, encoder push activates,
                        button short = Back (up a level), button long = Home (jump to root)
LIST_BROWSE           — encoder rotate moves selection up/down, encoder push opens/selects,
                        button short = Back (exit list), button long = Home
PARAM_EDIT_VALUE     — int / float / options / bool. Encoder rotate changes the value (± step,
                        accelerated for int/float, next/prev choice, or flip); encoder push
                        confirms + sends. Button short = cancel, button long = Home.
PARAM_EDIT_SEGMENTED — date / time. Encoder rotate changes the highlighted field's value;
                        encoder push short = move to next field (wraps), encoder push long =
                        confirm whole value + send. Button short = cancel, button long = Home.
PARAM_EDIT_TEXT      — string. Encoder rotate cycles the next character through the param's
                        charset; encoder push short = accept it and advance (extends the
                        string), encoder push long = commit the string + send. Button short =
                        delete the last character; button long = cancel the whole edit
                        (Home is not directly reachable from this mode — see §5.2.1).
TEXT_SCROLL          — encoder rotate scrolls message text, button short = Back (to the
                        Messages list), button long = Home
```

The three `PARAM_EDIT_*` modes are what §5.2.1's per-type table actually
drives: `ParamEditScreen` picks one based on the current param's `type`
when it's entered, and `AppController` sets `input_manager`'s mode to
match — `input_manager` itself has no per-task or per-param knowledge, just
these mode definitions. Note that in every mode except `PARAM_EDIT_TEXT`,
the button's meaning is the same fixed rule (short = cancel/Back, long =
Home) — only `PARAM_EDIT_TEXT` overrides it, and only because free-text
entry needs a delete affordance the hardware has no other control for.

A **factory-reset combo** — encoder push-button and the dedicated button
held down together for several seconds — is recognized globally,
independent of the current mode, and is handled separately from this table
(§11.4); it's a deliberate two-control gesture specifically so it can't be
triggered by an ordinary single long-press in any mode above.

`input_manager` owns the button/encoder ISRs (debounced, with quadrature
decoding for the KY-040) and just emits mode-appropriate semantic events
(`EncoderDelta(n)`, `EncoderPressShort`, `EncoderPressLong`,
`ButtonPressShort`, `ButtonPressLong`, `ResetComboDetected`) onto the event
bus; it doesn't know what screen is active. `AppController` is the only
thing that maps semantic events to actions, which keeps `input/` reusable
and testable in isolation.

#### 6.2.1 Timing constants

The short-vs-long and debounce thresholds this all depends on are starting
defaults, not measured values — real hardware feel (mechanical contact
bounce, how deliberate a "long press" needs to feel in the hand) can only
be tuned by testing. They're centralized in one header,
`input/input_timing.h`, specifically so retuning after that testing is a
one-file change instead of a hunt through `input_manager`/`button_driver`/
`encoder_driver`:

```cpp
// input/input_timing.h — starting defaults; expect to retune after
// hands-on testing on real hardware. Change here only.
namespace input_timing {

// Button (dedicated Back/Home) and encoder push-button short-vs-long
// press thresholds — kept as separate constants so they can diverge later
// even though they start equal (they're different physical switches).
constexpr uint32_t kButtonLongPressMs = 600;
constexpr uint32_t kEncoderLongPressMs = 600;

// Mechanical contact debounce, both controls are ordinary tactile switches.
constexpr uint32_t kButtonDebounceMs = 30;
constexpr uint32_t kEncoderDebounceMs = 30;

// Factory-reset combo (§11.4): encoder push-button + dedicated button held
// together. Deliberately well above kButtonLongPressMs/kEncoderLongPressMs
// so it can't trigger from an ordinary long-press in either control alone.
constexpr uint32_t kFactoryResetHoldMs = 5000;

} // namespace input_timing
```

### 6.3 Task execution flow

```
Encoder pushed on a task icon
  → if task has encoderAdjustable params: enter the matching PARAM_EDIT_*
    mode (§5.2.1/§6.2) for the first param, wait for confirm; repeat for
    each remaining param in order; a cancel at any step discards the whole
    sequence back to NAVIGATE with nothing sent
  → else: send immediately
AppController → net/task_publisher.send(task, paramValues)
  → builds { ...task.payload, ...paramValues, requestId } JSON
  → wraps it as a CoreEvent on mqtt.topics.messages:
      { componentId: <node id>, eventType: "task", value: <requestId>,
        metadata: { taskId, expectsResult, payload: <the JSON above> } }
    (value = requestId so every invocation is a state *change* for the
    server's State Engine — a repeated "lights on" would otherwise be
    dropped as a heartbeat)
  → net/mqtt_client.publish(messagesTopic, event, qos)
  → if MQTT not currently connected: drop it and show a UI error
    immediately (confirmed — no local command queue/replay: sending a
    stale "lights on" minutes later on reconnect would be worse than
    telling the user it didn't send)
  → if task.expectsResult is false (the default): show a lightweight
    "sent" acknowledgment right away and return to NAVIGATE — no waiting,
    no correlation, no timeout logic engaged
  → if task.expectsResult is true: wait for a correlated reply (below)
Backend executes the task and, only for expectsResult tasks, sends a reply
  command on mqtt.topics.commands:
      { action: "task_result", correlationId: <requestId>,
        result: { status: "ok" | "error", message, ...task-specific } }
  — its contents are task-specific and the UI can act on them (e.g. show a
  returned value/message), not just render a generic success/fail icon
net/mqtt_client's subscription callback matches correlationId → publishes
  TaskResultEvent (success/failure/custom payload), or a short client-side
  timeout (e.g. 5s with no result) publishes a TaskResultEvent(timeout)
ui shows a transient toast/status then returns to NAVIGATE
```

### 6.4 Inbound message flow

```
Server sends a command on the device's mqtt.topics.commands topic:
    { action: "message", text, icon?, id?, timestamp? }
  (revised: `messages` is the device → server direction, §5.1)
  → net/mqtt_client's subscription callback fires with the raw payload
  → parses { text, icon?, id?, timestamp? }
  → storage/fs_store persists to a small on-disk queue (bounded, e.g. last 50)
  → publishes MessageReceivedEvent
app/message_center updates the "message_inbox" binding
ui, if currently on the Messages folder/list, refreshes live; otherwise
  shows a small unread-count badge on the Messages icon
```

## 7. Proposed folder structure

```
/
├── docs/
│   └── ARCHITECTURE.md            ← this document
├── platformio.ini
├── data/                          ← LittleFS image contents
│   ├── config/
│   │   └── default_config.json
│   └── assets/
│       └── icons/                 ← runtime-pushed icon PNGs land here (§5.3 pt.5), not build-time ones
├── assets/
│   ├── icons_src/
│   │   └── icons8/                ← source PNGs downloaded from Icons8, checked in (§12.2)
│   │       ├── icon_light_on.png
│   │       └── icon_volume.png
│   └── icons_manifest.json        ← id → source URL/license/file mapping (§12.3)
├── scripts/
│   └── build_icons.py             ← pre-build hook: manifest↔config check + LVGL conversion (§12.5)
├── include/
├── lib/                           ← vendored/local libraries, if any
├── src/
│   ├── main.cpp                   ← setup()/loop(), task creation
│   ├── app/
│   │   ├── app_controller.cpp/.h  ← top-level state machine, screen routing
│   │   ├── menu_model.cpp/.h      ← in-memory menu tree built from config
│   │   ├── task_executor.cpp/.h   ← orchestrates param collection → send
│   │   ├── message_center.cpp/.h  ← inbox state, unread tracking
│   │   └── system_actions.cpp/.h  ← handles `action` nodes: reprovision,
│   │                                  restoreDefaultConfig, showAbout (§5.2)
│   ├── config/
│   │   ├── config_manager.cpp/.h  ← load/validate/stage/commit/rollback
│   │   └── config_schema.h        ← field names, defaults, limits
│   ├── ui/
│   │   ├── ui_manager.cpp/.h      ← LVGL init, display flush glue, screen stack
│   │   ├── theme.cpp/.h
│   │   ├── screens/
│   │   │   ├── icon_carousel_screen.cpp/.h
│   │   │   ├── list_screen.cpp/.h
│   │   │   ├── message_screen.cpp/.h
│   │   │   ├── param_edit_screen.cpp/.h
│   │   │   └── provisioning_screen.cpp/.h  ← QR code (lv_qrcode) + status text
│   │   ├── widgets/
│   │   │   ├── icon_widget.cpp/.h
│   │   │   ├── list_item_widget.cpp/.h
│   │   │   ├── value_stepper_widget.cpp/.h   ← int/float/options/bool (§5.2.1)
│   │   │   ├── segmented_field_widget.cpp/.h ← date/time
│   │   │   └── char_wheel_widget.cpp/.h      ← string entry
│   │   └── generated/              ← gitignored; icon C arrays + registry,
│   │                                  regenerated by scripts/build_icons.py (§12.5)
│   ├── input/
│   │   ├── input_manager.cpp/.h   ← mode FSM, event dispatch, factory-reset
│   │   │                            combo detection (§6.2, §11.4)
│   │   ├── input_timing.h         ← tunable ms thresholds, §6.2.1 — the
│   │   │                            one file to edit after hardware testing
│   │   ├── button_driver.cpp/.h   ← single button: debounce, short/long press
│   │   └── encoder_driver.cpp/.h  ← KY-040: quadrature decode + debounce,
│   │                                  push-button short/long press
│   ├── net/
│   │   ├── net_manager.cpp/.h     ← Wi-Fi connect/reconnect
│   │   ├── mqtt_client.cpp/.h     ← wraps esp_mqtt_client: connect (TLS +
│   │   │                            credentials from NVS), LWT/status,
│   │   │                            reconnect backoff, subscribe management,
│   │   │                            publish helpers, per-topic callback routing
│   │   └── task_publisher.cpp/.h  ← builds task payload from config + live
│   │                                param values, requestId correlation
│   ├── provisioning/
│   │   ├── ble_provisioning.cpp/.h   ← wraps wifi_provisioning/protocomm,
│   │   │                                registers the custom "mqtt_config"
│   │   │                                endpoint alongside the built-in
│   │   │                                Wi-Fi one
│   │   └── provisioning_manager.cpp/.h ← provisioned-flag check at boot,
│   │                                      session PoP generation, teardown
│   │                                      once credentials are confirmed
│   ├── storage/
│   │   ├── nvs_store.cpp/.h       ← secrets: MQTT username/password or
│   │   │                            TLS client cert+key, Wi-Fi creds
│   │   └── fs_store.cpp/.h        ← LittleFS helpers: config, icons, message queue
│   ├── hal/
│   │   ├── board_pins.h           ← all GPIO assignments in one place
│   │   └── display_hal.cpp/.h     ← LovyanGFX/panel init, backlight, rotation
│   └── util/
│       ├── event_bus.cpp/.h
│       └── logger.h
└── test/                          ← PlatformIO native/unity tests
    ├── test_config_manager/       ← JSON parsing/validation, no hardware needed
    ├── test_menu_model/
    └── test_input_fsm/
```

Rationale for the split: `app/` is the only layer allowed to know about
_both_ UI and network concepts — every other module is single-purpose and
mockable, which is what makes `test/` viable without hardware (config
parsing, menu tree building, and the input mode FSM are all pure logic and
can run under PlatformIO's `native` test environment).

## 8. Security notes

- The MQTT connection should run over TLS (`mqtts://`, port 8883) — decide
  now whether the device pins/trusts a specific broker CA cert (baked into
  `data/`, passed to `esp_mqtt_client`'s `cert_pem`) or skips verification.
  Given this is a local-network broker, cert pinning is the safer default
  and not much extra work, same reasoning as the original HTTPS plan.
- Authentication is now connection-level: MQTT username/password (or mutual
  TLS with a per-device client cert/key) presented at `CONNECT` time,
  replacing the REST login/token-refresh flow entirely. Credentials live in
  NVS, never in the JSON config.
- Because there's no longer an inbound HTTP endpoint to protect, the
  equivalent control is **broker-side topic ACLs**: this device's
  credentials should only be authorized to publish on its own
  `devices/<id>/cmd` topic and subscribe to its own `messages`/`config`/
  `cmd/result` topics. Without per-device ACLs, any client with broker
  access could publish fake messages/config to this device or spy on
  others' topics. Provisioning those ACLs is an operational/backend
  concern handled by a separate process, out of scope for this document —
  the firmware side only needs its BLE-delivered credentials (§11.2) to
  already be scoped correctly by whatever issues them.
- The LWT (Last Will and Testament) on `mqtt.topics.status` doubles as a
  security/observability signal: an unexpected disconnect publishes
  "offline" immediately, so the backend doesn't have to poll or guess.
- BLE provisioning (§11) uses `protocomm`'s **Security2** (SRP6a,
  username+password — confirmed) to encrypt the session, never Security0
  (unencrypted) or a fixed Security1 PoP, which would put Wi-Fi and MQTT
  credentials on the air in the clear or rely on a guessable secret. The
  session username+password pair is generated fresh per provisioning
  session and shown as a QR code on the device's own display, not printed
  on a label — this avoids a fixed secret derivable from the device's
  advertised MAC/name, and means no manufacturing-time secret provisioning
  step is needed.
- The BLE GATT stack and `protocomm` service are torn down as soon as
  provisioning succeeds (not left running), both to free RAM back to
  LVGL/MQTT (BLE+Wi-Fi coexistence is a tight budget on a single-core C6)
  and to shrink the attack surface once the device is in normal operation.

## 9. Milestones

1. **Skeleton** — PlatformIO project boots, display initialized, LVGL
   renders a static screen. Validates the hardware assumptions in §2.
2. **Menu engine** — load `default_config.json`, build `menu_model`, render
   IconCarouselScreen with KY-040 rotate/push navigation (§6.2).
3. **Provisioning** — BLE `protocomm` service comes up when unprovisioned,
   ProvisioningScreen renders a QR code, Wi-Fi credentials + MQTT broker
   config land in NVS/config from a minimal test companion app, device
   reboots into normal operation. (Can be stubbed with hardcoded Wi-Fi/MQTT
   values for Milestones 1–2 so UI/menu work isn't blocked on this.)
4. **Networking** — Wi-Fi connect, `mqtt_client` connects with TLS +
   credentials, `task_publisher` publishes a real command on encoder-push
   for a no-param task.
5. **Rotary encoder** — ParamEditScreen + `encoderAdjustable` params;
   encoder integrated into `input_manager`'s mode FSM.
6. **MQTT messaging** — subscription to the messages topic, MessageScreen
   with encoder-driven scrolling, Messages folder binding, command-result
   correlation (`requestId`) for task-execution toasts.
7. **Remote config update** — retained-message config push, staging/
   validation/rollback path.
8. **Polish** — unread badges, transient toasts for task results, icon
   delivery for pushed messages, recovery-mode config reset, re-enter-
   provisioning trigger for factory reset.

## 10. Open questions

Everything that was open as of the last few revisions has now been decided
— input hardware (§2), reboot behavior (§5.3), icon format/license/style
(§3, §12), BLE security scheme and provisioning source of truth (§8, §11),
result correlation (§5.2, §6.3), disconnected-command handling (§6.3), QoS
per topic (§5.1), and input timing defaults (§6.2.1, deliberately left as
tunable constants rather than fixed values). Broker-side topic ACL
provisioning (§8) is out of scope here — it's being handled as a separate
effort, not a firmware concern.

Nothing currently outstanding. Add items here as new ones come up during
implementation.

## 11. Initial setup / provisioning

The companion app that drives this flow is a **separate project, built in
parallel** — implementation is out of scope for this document, but the
wire contract it integration-tests against is not:
[BLE_PROVISIONING_PROTOCOL.md](BLE_PROVISIONING_PROTOCOL.md) is the
authoritative spec both sides build against, and [POC.md](POC.md)'s
provisioning step is the firmware-side reference implementation of it.
Everything below covers the firmware-side flow/state machine; §11.3 points
at the contract doc for the schema itself.

### 11.1 Why BLE, not SoftAP+HTTP or a "default" broker

The device ships with zero network identity: no Wi-Fi credentials, no MQTT
broker address. Two connectivity-bootstrap approaches were considered
(§ discussion prior to this doc):

- **SoftAP + a temporary local HTTP captive portal** — works from any
  browser, no companion app needed, but means the device runs an HTTP
  server, even if only transiently. Given the deliberate move away from
  HTTP everywhere else in this design (§3), and that a companion app was
  already planned for other reasons, this was passed over.
- **A "default" MQTT broker for setup** — not viable as the bootstrap
  transport itself: the device has no IP network at all until Wi-Fi is
  configured, so it cannot reach any broker, default or otherwise, until
  after the problem this step is meant to solve is already solved.
- **BLE provisioning (chosen)** — the device never opens a Wi-Fi AP or runs
  HTTP. A companion app talks to it over BLE GATT using ESP-IDF's
  `wifi_provisioning` component (built on `protocomm`), which handles the
  encrypted session, the Wi-Fi-credential endpoint, and connection-status
  reporting out of the box. We add one custom endpoint to the same session
  for MQTT broker config, so both are collected in a single provisioning
  flow.

### 11.2 Flow

1. **Entry**: at boot, `provisioning_manager` checks a `provisioned` flag in
   NVS. If unset (first boot, or after a factory reset — see §11.4), or if
   the "Reset Wi-Fi/MQTT" menu action or the hardware factory-reset combo
   is triggered (§11.4), `AppController` hands off to the provisioning path
   instead of the normal menu.
2. **Session start**: `provisioning_manager` generates a random Security2
   (SRP6a) username+password pair for this session only, and
   `ble_provisioning` starts `protocomm` over BLE, advertising a device
   name that includes a short unique suffix (e.g. derived from the MAC) so
   multiple in-range devices are distinguishable.
3. **Discovery**: `ui/screens/provisioning_screen` renders a QR code
   (`lv_qrcode`) encoding the device name + session credentials +
   transport, plus the same info as human-readable fallback text. No
   printed label or manufacturing-time secret is required — this is
   regenerated fresh every time provisioning mode is entered.
4. **Handshake**: the companion app scans the QR (or the user enters the
   fallback text), connects over BLE, and completes `protocomm`'s Security2
   handshake (encrypted session key established either way).
5. **Wi-Fi**: the app sends SSID/password over `wifi_provisioning`'s
   built-in endpoint; the device attempts to connect and reports
   success/failure back over the same BLE session (this status reporting
   is provided by the component, not something we build).
6. **MQTT**: the app sends **all** broker connection details — host, port,
   TLS flag, clientId, username, password (or client cert/key), and topics
   — over our custom `mqtt_config` endpoint (confirmed: nothing broker-
   related is preset in firmware; it's entirely delivered this way).
   `config_manager` writes the non-secret fields into `active.json`'s
   `mqtt` block and `storage/nvs_store` writes the password (or client
   cert/key) — same split as steady-state config (§5, §8).
7. **Verification (recommended)**: before declaring success, attempt a
   real MQTT connect with the newly-provisioned details and report that
   result back over BLE too — catches a typo'd broker address or bad
   credentials while the user still has the companion app open, instead of
   surfacing a cryptic failure on the device screen after BLE has already
   torn down.
8. **Teardown**: on confirmed success, `provisioning_manager` sets the
   `provisioned` flag, tears down `protocomm`/BLE completely (§8), and
   reboots into normal operation.

### 11.3 Protocol versioning

The custom `mqtt_config` BLE endpoint's payload shape is a contract between
two independently-built things — this firmware and the separately-built
companion app (§11 intro) — so it needs its own version field (e.g.
`provisioningProtocolVersion`) from day one, separate from `configVersion`
(§5), so the app can detect "firmware too old/new for this app" rather
than failing on a malformed payload. The full wire-level contract —
service/characteristic UUIDs, the QR payload shape, and the exact
`mqtt-config`/`mqtt-status` request/response schemas — is specified
separately in
[BLE_PROVISIONING_PROTOCOL.md](BLE_PROVISIONING_PROTOCOL.md), since both
firmware and app developers need to reference it independently; this
section just covers *why* it's versioned, not the schema itself.

### 11.4 Factory reset / re-provisioning

Two ways to re-enter this flow, both clearing the `provisioned` NVS flag
(and the stored Wi-Fi/MQTT credentials) and rebooting into it:

1. **"Reset Wi-Fi/MQTT" menu action** (§5.2, an `action` node under
   Settings) — the normal path, available whenever the menu itself is
   reachable. Only clears provisioning state; leaves `active.json` (menu
   config) untouched.
2. **Hardware factory-reset combo** — the KY-040's push-button and the
   dedicated button held down together for `kFactoryResetHoldMs` (default
   5000ms, §6.2.1 — well above the ordinary long-press threshold so it
   can't trigger by accident, subject to retuning once there's real
   hardware to test against). This is the fallback for when the menu
   itself is unreachable (e.g. a bad Wi-Fi/MQTT state with no way to
   navigate to Settings), so it does a
   **full reset**: provisioning state *and* `active.json` both revert
   (the latter via the same restore performed by "Restore Default Config,"
   §5.3 point 4), rather than requiring two separate recovery gestures.

Because provisioning re-entry now has its own dedicated two-control combo
(distinct from any single-button long-press, which always means Home —
§6.2), there's no collision to design around between this and ordinary
navigation.

## 12. Icon asset pipeline (Icons8 → firmware)

Icon IDs in the config (`"icon": "icon_light_on"`) are just strings; this
section defines where the actual artwork behind those IDs comes from and
how it becomes the baked-in LVGL image table referenced in §3 and §5.3
point 5.

### 12.1 Licensing note (confirmed: free tier)

Icons8's free tier requires attribution (a visible credit/link) wherever
the icons are used; their paid "Enhanced License"/subscription removes
that requirement and is what unlocks bulk/API access. **Decision: free
tier.** Consequences of that choice, concretely:

- The device needs a visible credit to stay compliant — this is now the
  `"showAbout"` `action` node under Settings (§5.2), a static text screen
  reading something like "Icons by Icons8" (with the attribution link, if
  the About screen supports rendering one, or in accompanying
  documentation/packaging otherwise) — not just a code comment.
  **Status: implemented** in the POC — `showAbout` renders the credit and
  link on-device. See `poc/src/main.cpp`.
- No official Icons8 API access. **The paid tier has been explicitly ruled
  out** (decision reaffirmed when §12.2's acquisition method changed), so
  the official-API route described at the end of §12.2 is not available to
  this project unless that reverses.

### 12.2 Source acquisition — automated download (revised)

> **This section was revised.** It previously specified a deliberate
> **manual** per-icon download, on the grounds that Icons8's ToS doesn't
> permit automated download outside their official paid API. The project
> now **automates** the download at build time, while remaining on the free
> tier. Recorded here in full, including the part that did not get
> resolved, so nobody later mistakes this for an oversight.

**What changed.** `poc/scripts/build_icons.py` reads the manifest and
fetches each icon's PNG from `img.icons8.com` during the build, caching it
under `data/icons_src/icons8/`. An icon is specified either by its Icons8
page URL (fetched by numeric ID — exactly that icon) or by a bare name
(fetched within the manifest's `style` — visually consistent across a set).
Both forms are tinted by `iconColor` (§12.8). A download happens only when
the PNG is absent — see the manual path below.

**The unresolved part — accepted risk, not a solved problem.**
`img.icons8.com` is the website's own CDN, not the official API. §12.1
rules out the paid tier, and the free tier has no API access, so this
automation has **no licence term that affirmatively permits it**. The
project has accepted that risk knowingly. Two things follow:

- This is the single change to revisit if Icons8 ever objects, changes
  their CDN, or the project's risk posture changes. Only
  `resolve_download_url()` in `build_icons.py` would need replacing.
- The free-tier **attribution obligation is entirely unaffected** by any
  of this and remains mandatory — see §12.1 and the `showAbout` screen.
  Automating acquisition does not dilute the attribution requirement; if
  anything it makes it more load-bearing, since it is now the project's
  only visible act of compliance.

**The manual path still exists, and needs no opt-in.** The rule is simply
that the file on disk wins: an icon whose PNG is already present is used
as-is and never re-downloaded or overwritten. Dropping a file into the
source tree is therefore the whole manual workflow — custom artwork,
separately-licensed assets, or this section's original per-icon manual
acquisition, all with no flag to set. Deleting a PNG is what re-fetches
it.

**Style selection** still works exactly as before:

1. Browse `icons8.com/icons/set/ios-filled` — **`ios-filled` is the
   project's default style** (flat, few-color, compresses well as indexed
   color — §12.4), configurable via `icons_manifest.json`'s `style` field
   (§12.3) if that ever changes. This is a **build-time** choice, not a
   runtime one: default icons are C arrays baked in at compile time
   (§12.4), so changing the style means re-running this pipeline and
   reflashing, not editing the runtime JSON config (§5) — the menu config
   only ever references icon *IDs*, never a style.
2. Add an entry to `icons_manifest.json` (§12.3) giving the icon's `id`,
   its `source` (page URL or bare name), and its `license` — this is what
   lets anyone later answer "is this icon actually licensed for what we're
   doing with it" without re-deriving it from memory.
3. Build. The PNG is downloaded to `data/icons_src/icons8/<icon_id>.png`
   at `sourceExportSize`, converted, and registered automatically.

An icon entry may set its own `"size"` (e.g. 28 for the Volume page's small
phone/computer icons); otherwise `sourceExportSize` applies.

`sourceExportSize` is both the download size **and** the final rendered
size — LVGLImage.py does not resize, so the source PNG's dimensions are
what get baked in. (This corrects an earlier assumption here that a large
canonical export would be downscaled at build time; it isn't.)

PNGs are fetched only when missing, so builds are offline-safe once the
artwork is present and the network is not touched on every rebuild.
Deleting a PNG is what re-fetches it.

The pipeline fails the build loudly rather than shipping wrong art: an
unknown icon name, a duplicate icon id, or a response that isn't a PNG (Icons8 answers unrecognised
requests with an HTML page, which is checked for explicitly).

If the paid tier is ever adopted after all, the *only* change that should
be made is pointing `resolve_download_url()` at Icons8's **official API**
with a valid key — the manifest and everything downstream stays identical.

### 12.3 Manifest (`icons_manifest.json`)

```jsonc
{
  "style": "ios-filled",
  "sourceExportSize": 128,
  // Tints BOTH source forms below. Defaults to FFFFFF because the UI is a
  // permanent dark theme (§12.8) and Icons8 icons default to solid black.
  "iconColor": "FFFFFF",
  // Icons8 free tier requires a visible credit (§12.1); the build fails
  // without this and renders it on the showAbout screen (§5.2).
  "attribution": "Icons by Icons8 - https://icons8.com",
  "icons": [
    {
      "id": "icon_light_on",
      // Bare name: fetched in "style" above, tinted with "iconColor".
      "source": "light-on",
      "license": "free-attribution",
      "file": "assets/icons_src/icons8/icon_light_on.png"
    },
    {
      "id": "icon_volume",
      // Page URL: fetched by numeric ID. Also tinted — add
      // "preserveColors": true to keep an icon's own colours (§12.8).
      "source": "https://icons8.com/icon/<id>/speaker",
      "color": "FFD54F",  // optional per-icon override of "iconColor"
      "license": "free-attribution",
      "file": "assets/icons_src/icons8/icon_volume.png"
    },
    {
      "id": "icon_custom",
      // Hand-placed art needs no flag: the file exists, so it is used as-is
      // and never downloaded. "source" is provenance only here.
      "source": "in-house artwork, not from Icons8",
      "license": "internal",
      "file": "assets/icons_src/icons8/icon_custom.png"
    }
  ]
}
```

`source` replaces the earlier `sourceUrl` and accepts both forms. `file` is
optional and defaults to `<icons_src>/<id>.png`. The POC uses `data/` rather
than `assets/` for this tree, because that's the directory PlatformIO builds
the LittleFS image from.

The manifest is checked into the repo alongside the source PNGs. It's the
single place that answers "what icons exist, where did each one come from,
and under what license" — the build script (§12.5) treats it as the
authoritative list, not a directory listing of `assets/icons_src/`.

### 12.4 Conversion to runtime format

Two runtime sizes are generated from each 128×128 source, both via Pillow
resize at build time rather than downloading twice:

| Use                          | Size  |
| ----------------------------- | ----- |
| `IconCarouselScreen` artwork  | 64×64 |
| `ListScreen`/message-row icons | 24×24 |

Each resized bitmap is then run through LVGL's official image converter
(`LVGLImage.py`, from the `lvgl/lvgl` repo's `scripts/` — used directly
rather than the Node `lv_img_conv` package, since PlatformIO's own build
scripting is already Python/SCons and this avoids adding a second language
toolchain just for icons) to produce an `lv_img_dsc_t` C array per icon.

Color format is picked per the chosen style, not fixed globally: flat,
few-color styles (recommended, §12.2) compress well as **indexed 4bpp or
8bpp + alpha**, which matters on a device with limited flash — a style
with gradients/photographic detail would force `true-color + alpha`
(roughly 4× the size per icon) and should be avoided for that reason as
much as for visual consistency.

### 12.5 Build integration

```
platformio.ini:
  extra_scripts = pre:scripts/build_icons.py
```

`scripts/build_icons.py` runs before every firmware build and:

1. Loads `icons_manifest.json` and confirms every source PNG it
   references exists.
2. Walks `data/config/default_config.json`'s `menu` tree (recursively,
   through `children`) collecting every `icon` value in use, and fails
   the build with a clear error listing any icon ID that has no manifest
   entry — this is what keeps the config and the icon set from silently
   drifting apart as tasks/folders are added.
3. Resizes each source PNG to the two sizes in §12.4 and runs the LVGL
   converter over each, writing output into `src/ui/generated/icons/`
   (gitignored — always regenerated, never hand-edited or committed).
4. Emits a generated lookup table, `icon_registry_gen.c/.h`, mapping each
   icon ID string to its `lv_img_dsc_t*` pair (carousel + list size) —
   this is the "baked-in LVGL image table" §5.3 point 5 already refers to
   for resolving `icon` IDs from config.

Because generated files are gitignored and rebuilt from the manifest +
source PNGs every time, there's exactly one source of truth for icon
artwork (`assets/icons_src/` + `icons_manifest.json`), not two.

### 12.6 Adding a new icon (day-to-day workflow)

1. Pick the icon on `icons8.com` in the project's chosen style (§12.2).
2. Download the 128×128 PNG, save as
   `assets/icons_src/icons8/<icon_id>.png`.
3. Add the entry to `icons_manifest.json`.
4. Reference `"icon": "<icon_id>"` in the JSON config (§5).
5. Build — `scripts/build_icons.py` regenerates the C tables and fails
   loudly if the manifest and config have drifted (step 2 of §12.5).

### 12.7 Relationship to server-pushed icons

This pipeline only covers icons baked in at build time — the set the
burned-in `default_config.json` can reference offline. It's unrelated to
the runtime path in §5.3 point 5 for icons delivered alongside a pushed
message or config update (`/assets/icons/<id>.bin` on LittleFS, raw
bytes, no build-time conversion involved): those never touch Icons8 or
this manifest, since they arrive from the server at runtime, not from a
build step.

### 12.8 Dark theme and icon colour

**The UI is dark-themed, always** — there is no light mode and no runtime
toggle. Implemented as `LV_THEME_DEFAULT_DARK` in `lv_conf.h`, which LVGL
applies inside `lv_display_create()`, so every screen built with
`lv_obj_create(NULL)` inherits it without per-screen styling.

Two consequences that are easy to get wrong, both of which are silent
visual failures rather than build errors:

- **Icons must be tinted at fetch time.** Icons8 serves icons as solid
  black by default — *including* catalogue entries named "…-logo", which
  are monochrome silhouettes, not the brand's colour mark. Against a dark
  background that is invisible. The manifest's `iconColor` (default
  `FFFFFF`) is therefore applied to **both** forms of `source`: as a path
  segment for the name form, and as a `color` query parameter for the
  page-URL form. Per-icon `color` overrides it; per-icon
  `preserveColors: true` disables tinting for genuinely multi-colour
  artwork.

  Do not assume an icon carries its own contrast because of what it is
  named — check the actual pixels. The POC shipped a black-on-black
  welcome icon for exactly this reason: "google-logo" was assumed to be
  the colour mark, and it is not.

- **Appearance settings only apply at download time.** `iconColor`,
  per-icon `color`, `preserveColors`, `style` and `sourceExportSize` shape
  the request URL, and the fetch only happens when a PNG is absent (§12.2).
  Editing one of them therefore does nothing to an icon that already has a
  file — delete the PNG to re-fetch it. The build names this on every line
  it prints, because a setting that appears not to work is otherwise very
  hard to distinguish from a setting that is broken.
- **QR codes stay dark-on-light and need their quiet zone.** The
  provisioning QR is the one surface that must *not* follow the theme:
  inverted codes are out of spec and a meaningful share of phone scanners
  reject them, which would break provisioning outright. Against a dark
  screen the surrounding light margin also stops being incidental, so
  `lv_qrcode_set_quiet_zone()` must be enabled — LVGL leaves it off by
  default, which lets the modules run to the canvas edge.
