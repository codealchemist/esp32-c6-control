# ESP32-C6 Remote Control Panel — Proof of Concept Build Guide

## 1. Goal & scope

Before building the real menu-driven app described in
[ARCHITECTURE.md](ARCHITECTURE.md), get one small firmware image burned
onto the actual Waveshare ESP32-C6-LCD-1.47 hardware that proves the
riskiest pieces work end-to-end: the display, an icon rendered through the
real build pipeline, the input hardware, BLE provisioning, and both
network legs (Wi-Fi + MQTT). Everything here is a **linear, one-shot test
sequence**, not the real app — there is no menu engine, no task dispatch,
no message inbox, no remote config update. Those come after this passes.

This is a separate, self-contained PlatformIO project at `poc/` (not
`src/`), the same pattern as `tools/display_smoke_test/` — so it can't
collide with the real firmware's eventual root-level `platformio.ini`/
`src/` layout from ARCHITECTURE.md §7.

## 2. What the POC does

A fixed sequence, run once per boot, that loops back to the start once you
dismiss the final status screen (§3 explains why):

```
┌─────────────┐  +2s   ┌──────────────┐  auto   ┌───────────────┐
│  Welcome    │───────▶│ Welcome +    │────────▶│  Device Info  │
│  screen     │        │ icon         │  ~2s    │  (scrollable) │
└─────────────┘        └──────────────┘         └───────┬───────┘
                                                          │ encoder push,
                                                          │ or 5s idle
                                                          ▼
                                                 ┌───────────────┐
                                                 │    About      │
                                                 │ (attribution) │
                                                 └───────┬───────┘
                                                          │ any press,
                                                          │ or 5s
                                                          ▼
                                                 ┌───────────────────┐
                                already          │  BLE provisioning │
                          provisioned? ──skip───▶│  QR on screen     │
                                   │              └─────────┬─────────┘
                                   │                         │ app pairs
                                   │                         ▼
                                   │              ┌───────────────────┐
                                   │              │  Connecting       │
                                   │              │  spinner + phase  │
                                   │              └─────────┬─────────┘
                                   │                         │
                                   ▼                         ▼
                          ┌──────────────────────────────────────┐
                          │  Connection status screen:            │
                          │  Wi-Fi: connecting/OK/failed          │
                          │  MQTT:  connecting/OK/failed          │
                          │  (waits here for ANY button/encoder   │
                          │  press to dismiss — no timeout)       │
                          └───────────────────┬────────────────────┘
                                               │ any press
                                               ▼
                                     loop back to Welcome
```

1. **Welcome screen** — static LVGL label, centered, something like "ESP32-C6
   Remote Control — POC". Proves display init + text rendering
   (`hal/display_hal`, LVGL, the confirmed ST7789 pins from ARCHITECTURE.md
   §2) work at all.
2. **Icon rendering, +2000ms later** — on the *same* screen (welcome text
   stays), one icon appears below/beside it. See §3 for why this should be
   a real Icons8-pipeline icon, not a placeholder.
3. **Device info, scrollable** — after another short pause, auto-advance to
   a scrollable text list (§3 has the recommended field list). Scrolling is
   driven by the KY-040 encoder rotation — this is the input hardware test,
   folded into a screen that needs to exist anyway rather than a separate
   throwaway diagnostic.
4. **Encoder push — or 5 seconds idle** — advances to the About screen.
   The push reuses "encoder push = confirm/activate" from the real input
   scheme (ARCHITECTURE.md §6.2) rather than inventing POC-only semantics.

   The idle timeout exists so the sequence is walkable on hardware that
   doesn't have the encoder wired up yet: without it, the very first
   screen requiring input is an unrecoverable stop, and the provisioning
   QR — the thing most worth testing early — is unreachable. It is an
   **idle** timer, not a fixed deadline: scrolling restarts it, so it
   never yanks the screen away mid-scroll once an encoder is attached.
   `kIdleAdvanceMs` in `main.cpp`. The screen states this on-screen and on
   Serial, since a screen that advances by itself with no warning reads as
   a crash.
5. **About screen** — ARCHITECTURE.md §5.2's `showAbout` action, showing
   the Icons8 attribution required by the free-tier licence (§12.1). In
   the real build this is reached from the Settings menu, which doesn't
   exist in the POC, so it sits in the fixed sequence instead. Dismissed
   by any press, or after the same 5s idle timeout as step 4 — it is the
   second and last input-gated screen before provisioning, so it needs the
   timeout too for the QR to be reachable without input hardware. The text
   is generated from `icons_manifest.json`'s
   `attribution` field, so it cannot drift from the licence record; it
   doubles as a check that the icon pipeline's generated header is being
   consumed correctly.
6. **BLE provisioning** — implements the firmware side of
   [PROVISIONING_PROTOCOL_V2.md](PROVISIONING_PROTOCOL_V2.md) exactly: a
   session token from the on-screen QR, AES-256-GCM over a plain BLE GATT
   service, carrying Wi-Fi credentials then MQTT config. This is
   the actual integration point with the companion app being built in
   parallel — not a stand-in test tool. Skipped entirely if the device is
   already provisioned from a previous loop (NVS flag) — see §3 for the
   force-re-provision escape hatch.
7. **Connect and report** — attempt Wi-Fi STA connect, then MQTT connect
   (TLS, credentials from the provisioning step or NVS).

   Both legs take seconds, so a **spinner screen** is shown throughout,
   captioned with the current phase (`Connecting to Wi-Fi`, the SSID, then
   `Connecting to MQTT` and the broker). On the provisioning path it
   replaces the QR the moment the app authenticates — the QR is spent at
   that point and leaving it up implies it's still needed.

   The spinner is only meaningful if it actually turns: these calls own the
   task for 15–20s each, so `netConnectWifi()` / `netConnectMqttAndTestEcho()`
   take an `onPoll` callback that pumps LVGL from inside their wait loops.
   Without it the animation freezes on the first frame and the device reads
   as crashed — worse than showing nothing.

   Then both results render on one status screen: `Wi-Fi: <SSID> — Connected (<IP>)` /
   `Failed (<reason>)`, and the same shape for MQTT.
8. **Wait for OK** — per your spec, *any* button press (short or long) or
   encoder push dismisses this screen — no auto-timeout, no distinction
   between the different press types here, unlike everywhere else in the
   real input scheme.
9. **Loop back to Welcome.** Re-provisioning is skipped on subsequent loops
   (step 6) unless forced, so you can re-run steps 1–3 and the
   connect-and-report step repeatedly without redoing BLE pairing every
   time — see §3.

## 3. Recommended additions

You asked what else would be good to have — here's what I'd fold in, and
why each one directly de-risks something the main architecture depends on
rather than being scope creep:

- **Use the real Icons8 pipeline for the one icon in step 2** (ARCHITECTURE.md
  §12), with a minimal `icons_manifest.json` containing just that one
  entry, rather than a hand-rolled placeholder bitmap. This is the first
  real exercise of `scripts/build_icons.py` and the LVGL conversion step —
  finding out that pipeline has a problem now, on one icon, is much
  cheaper than finding out after the full icon set is built.
- **LittleFS mount + read-back smoke test**, printed to Serial before the
  welcome screen even renders: mount LittleFS, read whatever's at
  `/config/`, print its size. The real config system (§5) and icon
  resolution (§5.3 point 5) both depend on LittleFS working, so confirming
  it mounts and reads correctly on this exact board/partition table early
  is cheap insurance.
- **MQTT pub/sub round-trip, not just connect.** After the MQTT connect
  succeeds, subscribe to one test topic and publish a small "hello" to it,
  confirming the subscription callback actually fires — a successful
  `esp_mqtt_client` connect doesn't prove pub/sub works if there's a broker
  ACL or topic typo, and that's a materially different failure mode worth
  distinguishing on the status screen (`MQTT: Connected` vs.
  `MQTT: Connected, no echo`).
- **Free heap (and PSRAM, if present) + firmware build timestamp**, shown
  on the device info screen. LVGL + BLE + Wi-Fi + MQTT coexisting on a
  single-core C6 with limited RAM was flagged as a real risk in
  ARCHITECTURE.md §2/§8 — seeing actual free-heap numbers this early is
  more useful than assuming it'll fit. The build timestamp is a cheap way
  to confirm you're looking at the flash you think you are, which matters
  once you're re-flashing this thing repeatedly.
- **Force-re-provision, two ways.** Without one, testing the provisioning
  step more than once means erasing NVS or reflashing every time, which
  makes the POC frustrating to actually use for its job — `loop()` skips
  the QR entirely once `provisioningIsProvisioned()` is true.
  1. **Boot-time button hold** — hold the dedicated button while powering
     on.
  2. **Type `r` + Enter in the serial monitor**, at any time. Clears the
     provisioning state and restarts, landing back on the QR.

  The serial command exists because the button hold assumes hardware that
  may not be wired up yet — and on a board with no encoder and no button,
  the button hold is unreachable, leaving a device that provisions exactly
  once and then can never show the QR again. It's polled from every
  blocking wait loop (`pumpFor`, `waitForInput`, and the device-info
  scroll loop), so it works on any screen, including the final status
  screen where an already-provisioned device otherwise parks indefinitely.

  It is deliberately *not* polled inside `provisioningRun()`: that path
  only runs when the device is **not** provisioned and the QR is already
  on screen, so there is nothing to reset.

  Both are POC-only shortcuts, simpler than the real app's menu-driven +
  hardware-combo scheme from ARCHITECTURE.md §11.4 (no Settings menu
  exists yet in the POC to host a "Reset Wi-Fi/MQTT" action).

## 4. Simplifications vs. the real architecture (deliberate, not oversights)

- **BLE provisioning uses Protocol v2 (token + AES-GCM)**, replacing the
  Security2/SRP6a design ARCHITECTURE.md §8/§11 originally called for — see
  PROVISIONING_PROTOCOL_V2.md §1 for why, and what it costs. The companion
  app is being built in
  parallel as its own project, and this POC is exactly what that app
  integration-tests against — not a Python tool standing in for it. The
  wire contract both sides implement against is
  [BLE_PROVISIONING_PROTOCOL.md](BLE_PROVISIONING_PROTOCOL.md); this POC's
  `provisioning.cpp/.h` (§6) is the reference implementation of the
  firmware side of that contract.
- **One fixed MQTT test topic** (e.g. `poc/<deviceId>/hello`), not the full
  `commands`/`config`/`messages`/`status`/`cmd/result` topic taxonomy from
  ARCHITECTURE.md §5.1 — this POC is validating the transport, not the
  application protocol on top of it.
- **No menu engine, task execution, message inbox, or remote config
  update.** Those are the actual point of the real app and come after this
  passes.
- **Flat code structure**, not the modular `app/`/`ui/`/`input/`/`net/`/
  `provisioning/`/`storage/` split from ARCHITECTURE.md §7 — a handful of
  files under `poc/src/` is enough for a fixed linear sequence; building
  the real module boundaries now would be premature for a one-shot
  validation script.

## 5. Prerequisites

| Item | Status |
|---|---|
| Display pins (ST7789) | Confirmed — see ARCHITECTURE.md §2 (MISO=5, MOSI=6, SCLK=7, CS=14, DC=15, RST=21, Backlight=22) |
| KY-040 + dedicated button pins | **Still needed from you** — same open item as ARCHITECTURE.md §2's `hal/board_pins.h` TBD, this POC can't be wired up without it |
| PlatformIO | CLI or VS Code extension, on your machine (this sandbox has no USB access to the device — same caveat as `tools/display_smoke_test`) |
| Libraries | LVGL v9, LovyanGFX, ArduinoJson v7 (for the tiny provisioning payload + manifest), ESP-IDF's `mqtt` component (`esp_mqtt_client`) and `wifi_provisioning`/`protocomm` components — same choices as ARCHITECTURE.md §3 |
| A broker to test against | Any MQTT broker reachable from your Wi-Fi, TLS-capable if you want to test that path too |

## 6. Project layout

This has been built — see `poc/` in the repo root. Actual layout:

```
poc/
├── README.md                     ← manual setup steps + honest risk ranking, read first
├── platformio.ini
├── data/
│   ├── icons_manifest.json       ← one entry, reusing ARCHITECTURE.md §12's schema
│   └── icons_src/icons8/
│       └── icon_test.png         ← synthetic placeholder — see README.md's setup step 5
├── scripts/
│   └── build_icons.py            ← scoped-down version of the real pipeline (§12.5)
└── src/
    ├── main.cpp                  ← the linear sequence in §2
    ├── pins.h                    ← display pins confirmed; input pins are
    │                                placeholders — see README.md setup step 4
    ├── input_timing.h            ← POC-scoped copy of ARCHITECTURE.md §6.2.1's constants
    ├── display.cpp/.h            ← ST7789 (LovyanGFX) + LVGL v9 glue
    ├── input.cpp/.h               ← KY-040 quadrature decode + button short/long press
    ├── device_info.cpp/.h        ← chip/heap/PSRAM/LVGL-version/build-stamp strings
    ├── net.cpp/.h                ← Wi-Fi connect + MQTT connect/pub/sub echo test
    ├── provisioning.cpp/.h       ← BLE GATT server, token+AES-GCM framing, Wi-Fi
    │                                and MQTT config, per PROVISIONING_PROTOCOL_V2.md
    ├── prov_crypto.cpp/.h        ← HKDF-SHA256 / AES-256-GCM / base64url (mbedTLS)
    └── generated/                ← gitignored; build_icons.py's output
```

## 7. Build / flash / monitor

**Read `poc/README.md` first** — there are one-time manual setup steps
(installing the icon converter's Python packages, confirming input pins)
before the first build. Once those are done, from
`poc/`:

```
pio run -t uploadfs   # writes icons_manifest.json + the built icon to LittleFS
pio run -t upload
pio run -t monitor
```

Re-run `uploadfs` whenever the manifest or source icon changes;
`scripts/build_icons.py` regenerates the LVGL C array on every `pio run`
per ARCHITECTURE.md §12.5's `pre:` hook, so a plain `upload` picks up icon
changes too as long as the manifest/source files were saved first.

## 8. Pass/fail checklist

- [ ] Every screen renders **dark** — light text on a dark background, no
      screen flashing white on entry (ARCHITECTURE.md §12.8)
- [ ] Welcome text renders correctly, centered, no corruption/tearing
- [ ] Icon appears ~2s after welcome text, at the right position, not
      distorted — confirms both LVGL image rendering and the real Icons8
      build pipeline (§3) produced a correct C array
- [ ] The icon is actually *visible* against the dark background, not a
      black-on-black smudge. If it is invisible, the icon was fetched by
      name without a light `iconColor`, or by URL as dark artwork —
      ARCHITECTURE.md §12.8
- [ ] Device info screen shows plausible values (real MAC, non-zero free
      heap, correct flash size, a build timestamp that matches when you
      actually flashed it)
- [ ] Encoder rotation scrolls the device info list smoothly in the
      expected direction (flag here if it's inverted — cheap HAL fix)
- [ ] Device info screen advances on its own after ~5s with no input, and
      the on-screen hint says so (this is what makes the sequence walkable
      before the encoder is wired up)
- [ ] Once an encoder IS attached: scrolling restarts the idle timer, so
      the screen does not advance out from under an active scroll
- [ ] Typing `r` + Enter in the serial monitor clears provisioning and
      restarts, landing back on the QR — test it specifically from the
      final status screen, which is where an already-provisioned device
      with no input hardware would otherwise be stuck for good
- [ ] Encoder push advances to the About screen
- [ ] About screen shows the Icons8 attribution and its link, fully legible
      and not clipped at 320x172 (this is the free-tier licence obligation,
      ARCHITECTURE.md §12.1 — a blank or truncated credit is a compliance
      failure, not a cosmetic one)
- [ ] Any press on the About screen advances to provisioning
- [ ] QR code scans on the first try from a real phone, at a normal
      arm's-length distance — it stays dark-on-light against the dark
      screen deliberately, and needs its light quiet zone intact
      (ARCHITECTURE.md §12.8). A code that only scans when held very close
      is a quiet-zone or module-scale problem, not a camera problem.
- [ ] `[SelfTest] === crypto OK ===` on Serial at boot — the v2 crypto
      matches PROVISIONING_PROTOCOL_V2.md §10's published vectors. Check
      this BEFORE debugging any client-side problem; if it fails, the
      firmware disagrees with the spec and nothing else will work
- [ ] QR is replaced by the spinner screen as soon as the app pairs, and
      the spinner keeps turning through both the Wi-Fi and MQTT attempts
      (a frozen spinner means the `onPoll` pump is not reaching LVGL)
- [ ] BLE provisioning: device advertises as `PROV_xxxxxx` with service
      UUID `8027151f-...` (PROVISIONING_PROTOCOL_V2.md §3), the app's
      HELLO is accepted (`[Prov] HELLO verified` on Serial), and Wi-Fi
      credentials then MQTT config are both accepted end-to-end — this is
      the actual integration test the contract doc exists for
- [ ] `mqtt-status` polling (BLE_PROVISIONING_PROTOCOL.md §6) returns
      `connecting` while attempting, then `connected`/`failed` with a
      correct `reason` — verify the app's polling loop actually terminates
      on both outcomes, not just the happy path
- [ ] Wi-Fi status renders correctly for both a success and a deliberately
      wrong password (test both)
- [ ] MQTT status renders correctly for connect success, connect failure
      (wrong host/port), and the pub/sub echo test specifically (§3)
- [ ] Any button or encoder press dismisses the status screen — confirm
      more than one input type works, not just the one you tried first
- [ ] Second loop skips BLE provisioning (already-provisioned path)
- [ ] Holding the button at boot forces re-provisioning even when already
      provisioned

## 9. After this passes

Move on to ARCHITECTURE.md's Milestones (§9) — the POC deliberately
overlaps Milestones 1 and 3–4 (skeleton, provisioning, networking), so
passing this checklist means those are largely already validated going
into the real modular build.
