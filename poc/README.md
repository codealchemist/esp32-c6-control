# POC firmware — setup & known risk areas

See [`../docs/POC.md`](../docs/POC.md) for what this validates, the full
sequence, and the pass/fail checklist. This file is just the mechanical
"how to get it to build" steps and an honest list of what's unverified.

## One-time manual setup (required before first build)

1. **Fetch dependencies once** so their sources exist locally. Either:
   - CLI: `pio pkg install`, or
   - VS Code + PlatformIO extension: open this folder (the one containing
     `platformio.ini`) in VS Code with the extension installed — it
     resolves the environment automatically, or click the ✓ **Build**
     button in the PlatformIO toolbar, which fetches `lib_deps` as needed
     the same way `pio pkg install` does. Either path writes to the same
     `.pio/libdeps/` referenced in step 2 below, since the extension is a
     GUI wrapper around the same PlatformIO Core underneath — nothing else
     in this file changes based on which one you use.
2. **Install the icon converter's Python dependencies.** LVGL's
   `LVGLImage.py` (driven by `scripts/build_icons.py` on every build) imports
   `pypng` and `lz4`, neither of which is a PlatformIO dependency. They must
   go into PlatformIO's *own* Python, not your system one:
   ```
   ~/.platformio/penv/bin/python -m pip install pypng lz4
   ```
   `build_icons.py` checks for both up front and prints this exact command if
   either is missing.
3. ~~**Create `include/lv_conf.h`.**~~ **Done** — `include/lv_conf.h` is now
   committed (LVGL's `lv_conf_template.h` with `#if 1`, `LV_COLOR_DEPTH 16`,
   `LV_USE_QRCODE 1`, and `LV_MEM_SIZE (48 * 1024U)`). Raise `LV_MEM_SIZE`
   if you see allocation failures in the serial log.

   Note that `platformio.ini` must carry `-Iinclude`: lvgl is built as a
   library, and PlatformIO does **not** put `include/` on the include path
   for library builds, so without that flag lvgl cannot find `lv_conf.h`
   even though the file is right where it belongs.
4. ~~**Generate real Security2 (SRP6a) credentials.**~~ **No longer a step.**
   Provisioning moved to Protocol v2 (token + AES-GCM,
   `../docs/PROVISIONING_PROTOCOL_V2.md`), which drops SRP6a entirely. The
   device generates a fresh 32-byte session token per provisioning attempt
   and puts it in the QR; nothing is baked into the image and no external
   tooling is involved.
5. **Confirm the input pins** in `src/pins.h` — KY-040 CLK/DT/SW and the
   dedicated button are placeholders, not your board's actual wiring.
6. ~~**Get one real icon.**~~ **Automated** — the build downloads it. See
   *Adding icons* below. The synthetic placeholder is gone;
   the pipeline now pulls real Icons8 artwork.

## Adding icons

Edit `data/icons_manifest.json`; menu items then refer to the icon by its
`"id"` in the config. Each icon's `"source"` takes either form:

```jsonc
// by Icons8 page URL — fetches that exact icon by its numeric ID
{ "id": "icon_night", "source": "https://icons8.com/icon/<id>/<name>" }

// by bare name — fetches within the manifest's "style", so a set of
// icons requested this way stays visually consistent
{ "id": "icon_night", "source": "crescent-moon" }
```

The two can return **different artwork for the same word**. Use the URL
form when a specific icon matters, the name form when style consistency
does. (The welcome screen uses a built-in font symbol, not an icon from
this manifest.)

**Icon colour under the dark theme.** `"iconColor"` (default `FFFFFF`)
tints **both** forms. This isn't cosmetic: Icons8 icons default to solid
black — including the entries named "…-logo", which are monochrome
silhouettes rather than the brand's colour mark — and the UI is
permanently dark, so an untinted icon is a black shape on a near-black
screen.

- `"color": "RRGGBB"` on an individual icon overrides `"iconColor"`.
- `"preserveColors": true` on an individual icon disables tinting, for
  genuinely multi-colour artwork that flattening would destroy.

### When do icons download?

**One rule: the file on disk wins.** If
`data/icons_src/icons8/<id>.png` exists it is used as-is — never
re-downloaded, never overwritten. Only a *missing* file is fetched.

```
delete the PNG  →  next build re-downloads it
leave it alone  →  next build uses it, no network I/O
```

Consequences, all of which fall straight out of that one rule:

- **Hand-placed and hand-edited art is safe by default.** Drop your own
  PNG in at the icon's `file` path and it will be used and left alone. No
  flag to set. Keep `source` and `license` filled in for the §12.3 licence
  audit trail even when nothing downloads them.
- **Offline builds work**, provided the PNGs are present. Commit them.
- **A steady-state rebuild does no network I/O at all.**
- **Changing a setting does not re-fetch.** `iconColor`, a per-icon
  `color`, `preserveColors`, `style`, and `sourceExportSize` only affect
  the download, so an icon that already has a PNG keeps its old artwork.
  **Delete the PNG to pick the change up.** The build prints `using
  existing … (delete it to re-download)` on every such icon as a reminder.
- **A missing PNG plus no network is a hard failure**, naming the path to
  drop a file into.

Source PNGs live in `data/icons_src/icons8/` and are downloaded only when
missing — delete one to re-fetch it. See *When do icons download?* below.
`"sourceExportSize"` is both the download size and the final rendered
size (LVGLImage.py does not resize).

Mistakes fail the build loudly rather than silently shipping the wrong
art: an unknown icon name, a duplicate icon id, or a response that isn't
actually a PNG.

### Licensing — decided, with one accepted risk

**Decision: stay on the Icons8 free tier, and automate the download
anyway.** `../docs/ARCHITECTURE.md` §12.1/§12.2 have been revised to
record this.

The accepted risk, stated plainly so it isn't rediscovered later as a
surprise: `img.icons8.com` is the website's CDN, not Icons8's official
API. The free tier has no API access and the paid tier is ruled out, so
there is **no licence term that affirmatively permits this automation**.
If that ever needs undoing, `resolve_download_url()` in
`scripts/build_icons.py` is the only thing to change.

The **attribution obligation is unaffected and mandatory**, and is now
implemented: `"attribution"` in the manifest is generated into
`kIconAttribution` and rendered by the About screen (POC.md §2 step 5).
The build fails if the field is missing, and fails if it contains
non-ASCII — LVGL's built-in Montserrat font has no glyphs for the rest, so
an em dash would compile clean and then render as a blank box on-device.

## Build / flash / monitor

| Step | CLI | VS Code extension |
|---|---|---|
| Build | `pio run` | ✓ **Build** |
| Upload filesystem (icon/manifest) | `pio run -t uploadfs` | **Upload Filesystem Image** (Project Tasks → your env → Platform) |
| Flash | `pio run -t upload` | → **Upload** |
| Serial monitor | `pio run -t monitor` | 🔌 **Monitor** |

Upload the filesystem image *before* flashing at least once, same
requirement either way — that's what puts `icons_manifest.json` and the
built icon where `main.cpp`'s LittleFS smoke test and the icon registry
expect to find them.

### Flashing from VS Code, step by step

1. **Open the right folder.** Open `poc/` — the directory containing
   `platformio.ini` — not the repository root. The extension keys
   everything off that file; from the repo root it won't find the project
   and no Project Tasks appear.
2. **Wait for the first-run setup.** On a fresh checkout the extension
   downloads the pioarduino platform, the RISC-V toolchain, and
   `lib_deps`. This is several hundred MB and takes a while; the status bar
   shows progress. Nothing below works until it finishes.
3. **Install the icon converter's Python packages** (setup step 2 above)
   if you haven't. The extension uses the same `~/.platformio/penv`
   interpreter the CLI does, so doing it once covers both. The build prints
   the exact command if they're missing.
4. **Plug the board in via USB-C.** One cable does power, flashing, and
   serial — see the note below on why that matters.
5. **Open the PlatformIO sidebar** (the alien-head icon in the Activity
   Bar) → **Project Tasks** → **esp32-c6**.
6. **Run `Upload Filesystem Image`** once, under **Platform**. Skipping
   this on a first flash means LittleFS mounts empty and the smoke test
   fails in a way that looks like a firmware bug.
7. **Run `Upload`** under **General** — or the **→** arrow in the blue
   status bar at the bottom. This builds and flashes in one step; you do
   not need to hit Build first.
8. **Run `Monitor`** — or the **🔌** plug icon in the status bar. Baud is
   already set to 115200 in `platformio.ini`, so it connects without
   further configuration.

**Serial output and the single USB-C port.** This board wires its USB-C
directly to the C6's native USB Serial/JTAG peripheral (GPIO12/13, the
pins `src/pins.h` deliberately keeps free); there's no separate USB-UART
bridge. arduino-esp32 defaults `ARDUINO_USB_CDC_ON_BOOT` to `0`, which
points `Serial` at UART0 — pins with nothing attached. `platformio.ini`
therefore sets **both**:

```ini
-DARDUINO_USB_CDC_ON_BOOT=1
-DARDUINO_USB_MODE=1
```

Both are needed. `ARDUINO_USB_MODE=1` selects *hardware* CDC; the C6 has
only USB Serial/JTAG and no native USB OTG, so with `CDC_ON_BOOT=1` alone
the core takes its native-USB branch and fails to compile on `USBSerial`.

Without these the failure is quietly misleading: the flash succeeds, the
device runs, the screens render, and the monitor stays completely blank.
**This is unverified on real hardware** — if the monitor is blank *with*
them set, the board may have a UART bridge after all; remove both lines
and retry.

Two consequences of everything sharing one USB peripheral, both normal:

- The serial port **disappears and re-enumerates on every reset and
  flash**. The monitor will disconnect; reopen it. On macOS the device is
  typically `/dev/cu.usbmodem*`.
- If a port doesn't appear at all, hold **BOOT**, tap **RESET**, release
  **BOOT** to force the ROM bootloader, then Upload. Normally the
  auto-reset handles this and you never touch the buttons.

**Picking the port.** Auto-detection is usually right. If you have other
USB-serial devices attached and it grabs the wrong one, run **Devices** in
the PlatformIO sidebar to list ports, then pin it in `platformio.ini`:

```ini
upload_port = /dev/cu.usbmodemXXXX
monitor_port = /dev/cu.usbmodemXXXX
```

**Re-showing the pairing QR.** Once provisioning succeeds the QR is
skipped on every later boot. To get it back, **type `r` + Enter in the
serial monitor** — that clears the provisioning state and restarts. Works
on any screen, and needs no button or encoder wired up. (Holding the
dedicated button at boot does the same thing, if you have one attached.)

**Re-downloading an icon** needs no special task or env var — delete its
PNG under `data/icons_src/icons8/` and hit Build.

**If a build seems stuck on stale output**, run **Full Clean** under
Project Tasks → General. Plain **Clean** leaves `.pio/libdeps/` in place,
so it won't re-fetch libraries — which is usually what you want, since
`build_icons.py` needs `LVGLImage.py` from there.

## Known risk areas, ranked

A clean checkout now **compiles and links with no manual setup at all**
(RAM 37.9%, Flash 64.5%) — there is no longer any placeholder to fill in.
It has still **never been run on real hardware**, so everything below is
about runtime behaviour, not compilation. Roughly highest-to-lowest risk:

1. **`provisioning.cpp`** — still the biggest unknown, but a smaller one
   than before. The ESP-IDF components *are* available under the Arduino
   framework (that was the top worry, and it's resolved: it compiles and
   links). What remains is purely runtime: whether Arduino's `WiFi.h`
   having already brought up esp_netif/the event loop/esp_wifi conflicts
   with `network_prov_mgr` trying to own that same sequence. If Wi-Fi
   credential application silently does nothing, start there.
2. **Protocol v2 framing** (`sendFrame`/`handleFrame`/`handleFragment`) —
   the counter, replay and fragment-reassembly logic is ours rather than a
   vetted library's, and has never seen a real client. The crypto beneath
   it *is* checked at boot against the spec's published vectors — look for
   `[SelfTest] === crypto OK ===` on Serial before suspecting anything
   else. `esp_random()` is a true RNG here only because RF is already
   active when provisioning runs — keep that ordering.
3. **`display.cpp`**'s panel offset/rotation (`offset_x=34`,
   `setRotation(1)`) — translated from Waveshare's raw-SPI reference init
   into LovyanGFX's config struct, not verified visually.
4. **`main.cpp`**'s `lv_qrcode_*`/`lv_list_*` calls — compile against
   LVGL 9.5, but the rendered result is unverified.

Resolved since first build (kept here because the reasoning still matters):

- **Platform** — the official `espressif32` platform does not support
  `framework = arduino` on the C6 at all (its board manifest lists only
  `espidf`). `platformio.ini` now pins the community **pioarduino** fork.
- **Duplicate provisioning components** — arduino-esp32 3.3.x links both
  `wifi_provisioning` and `network_provisioning`, whose generated protobuf
  symbols collide, which needed a link-line hook to resolve. Moot since
  Protocol v2: neither component is linked any more, and the hook is
  deleted.
- **`build_icons.py`**'s `LVGLImage.py` CLI flags — verified against
  `--help` on the fetched LVGL 9.5; icons convert and compile.

Lower risk (standard, stable APIs): `net.cpp` (Wi-Fi/`esp_mqtt_client`),
`input.cpp` (the quadrature decode table is a widely-reused pattern),
`device_info.cpp`.
