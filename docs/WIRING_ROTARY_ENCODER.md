# Wiring the KY-040 rotary encoder

How to connect a KY-040 rotary encoder module (5 pins: `GND`, `+`, `SW`,
`DT`, `CLK`) to the Waveshare ESP32-C6-Touch-LCD-1.47 on a breadboard
(protoboard), and how to check that it works.

The firmware already supports it. A turn moves through the menu and adjusts
sliders, and a push opens or confirms (ARCHITECTURE.md §6.2). The pins are set
in `poc/src/pins.h`.

## What you need

- The ESP32-C6-Touch-LCD-1.47, with pin headers on its 22-pin connector.
- A KY-040 module.
- A breadboard.
- 5 jumper wires, male-to-female if the module plugs straight onto them,
  otherwise male-to-male through the breadboard.

## Connections

| KY-040 pin | Goes to board pin | ESP32-C6 GPIO | Notes |
|---|---|---|---|
| `GND` | `GND` | n/a | Any GND pin on either row |
| `+` | `3V3` (right row) | n/a | **3.3 V only.** Never `VBUS` (5 V from USB) or `VBAT` |
| `CLK` | `3` (left row) | GPIO3 | Encoder channel A |
| `DT` | `7` (right row, last pin) | GPIO7 | Encoder channel B |
| `SW` | `4` (left row) | GPIO4 | Push button, low when pressed |

### Board pinout

Waveshare's pinout diagram for this board (from the
[official docs page](https://docs.waveshare.com/ESP32-C6-Touch-LCD-1.47), shown
here by link, not copied into the repo):

![ESP32-C6-Touch-LCD-1.47 pinout (Waveshare)](https://docs.waveshare.com/assets/images/ESP32-C6-Touch-LCD-1.47-details-inter-27f8f0c6d70114f21e2fee2ea079c0c7.webp)

The two header rows run along the board's long edges. "Left" and "right" below
are as in the diagram: looking at the **component side** (the back, with the
microSD slot), USB-C port at the top. Flip the board over and they swap. The
silkscreen labels are mostly bare numbers:

```
  left row             right row
  VBUS                 VBAT
  GND        <- GND    GND
  TXD (16)             GND
  RXD (17)             3V3       <- encoder +
  RST                  SCL (19)
  1                    SDA (18)
  2                    13  (USB D+)
  3   <- CLK           12  (USB D-)
  4   <- SW            9
  5   (IMU INT1)       8   (BOOT)
  6   (IMU INT2)       7         <- DT
```

So the encoder uses three pins in the left row (GND, 3, 4) and two in the right
row (3V3, 7). Go by the printed labels; if the image above ever stops loading,
it's on the docs page linked above.

```
   KY-040                     ESP32-C6-Touch-LCD-1.47
 +---------+
 |  GND  o-+--------------------- GND
 |   +   o-+--------------------- 3V3  (right row)
 |  SW   o-+--------------------- 4    (GPIO4, left row)
 |  DT   o-+--------------------- 7    (GPIO7, right row)
 |  CLK  o-+--------------------- 3    (GPIO3, left row)
 +---------+
```

## Steps

1. **Unplug the board's USB cable.** Wire with the power off.
2. Seat the board and the KY-040 on the breadboard, each straddling separate
   rows so no two of their pins share a row strip.
3. Connect `GND` first, then `+` to **3V3**.
4. Connect `CLK` to pin 3 and `SW` to pin 4 (left row), and `DT` to pin 7
   (right row).
5. Check every wire against the table, especially that `+` goes to 3V3, not
   VBUS or VBAT. 5 V on these pins can damage the ESP32.
6. Plug the USB cable back in. The board boots normally; the encoder draws
   almost nothing.

## Why these pins

The 22-pin header only exposes a few GPIOs that the board doesn't already use
for the screen, touch, IMU or USB:

- **IO7:** completely free.
- **IO3 and IO4:** these are the TF (microSD) card's MISO and CS lines. They
  already have 10 kΩ pull-ups to 3V3 on the board, which suits the encoder:
  its outputs and button pull the line to GND. The catch is that **the TF
  card can't be used while the encoder is wired this way**. This firmware
  doesn't use the TF card.
- **Not used:**
  - `ESP_TXD`: the chip prints boot messages on it, and it could fight an
    encoder contact holding the line at GND.
  - `IMU_INT1` and `IMU_INT2`: driven by the IMU chip.
  - `8` and `9`: the BOOT button and the chip's boot-mode pin.
  - `IO1` and `IO2`: shared with the screen's SPI bus.

The KY-040 has its own 10 kΩ pull-ups on CLK and DT, powered from its `+` pin.
That's another reason `+` must be 3.3 V: the pull-ups then hold the lines at a
level the ESP32 can safely read. Many KY-040 boards leave the `SW` pull-up
unfitted, which is fine here because IO4 has the board's pull-up and the
firmware also turns on the internal one.

## Checking it works

Flash the firmware (see `poc/README.md`) and open the home menu:

| Action | What should happen |
|---|---|
| Turn right one click | The blue highlight moves to the next tile; at the end of a page the next page slides in |
| Turn left one click | The highlight moves back |
| Press the knob | The highlighted tile opens or runs, like a tap |
| Press on a dimmed or off screen | The screen wakes, and that press does nothing else |
| On the Volume page | Each click moves the slider by 5 |

The serial log (115200 baud) shows the same actions as `[Menu]` and `[Volume]`
lines.

## Troubleshooting

| Symptom | Likely cause / fix |
|---|---|
| Turning goes the wrong way | Swap the `CLK` and `DT` wires, or swap `PIN_ENCODER_CLK` and `PIN_ENCODER_DT` in `pins.h` |
| Nothing happens when turning | `+` not connected, so there are no pull-ups. Or CLK/DT are on the wrong header pins: check the silkscreen labels |
| One click moves two steps, or steps are skipped | Contact bounce on that unit. Tune the debounce values in `poc/src/input_timing.h` |
| The knob press does nothing | `SW` on the wrong pin, or the module's switch is faulty: check with a multimeter that SW goes to GND when pressed |
| Board won't boot with the encoder attached | Unplug the encoder and check that no wire touches pin `8` or `9` (boot-mode pins), and that `+` and `GND` aren't swapped |

## Shortcut button (optional)

A momentary push button that runs one menu item of your choice: the Lights
switch (toggles it), or a scene (first press runs it, the next turns the
lights off). Pick the item in **Settings → Shortcut button** on the panel; the
list only shows items the config marks as shortcut-able.

| Button leg | Goes to board pin | ESP32-C6 GPIO |
|---|---|---|
| one leg | `17` (left row, labelled **RXD**) | GPIO17 |
| other leg | `GND` | n/a |

- **No resistor needed:** the firmware turns on GPIO17's internal pull-up, and
  a press pulls the pin to GND.
- **Why pin 17:** it's UART0's receive pin, which is idle because the firmware
  uses USB for its serial console.
- **When it works:** from any menu page, and even when the screen is dimmed or
  off (it wakes the screen *and* runs). Settings pages, Volume and Device
  details close first, then it runs.
- **Feedback:**
  - Tasks and switches run in place, with a small pill at the bottom of the
    screen: a spinner, then ✓ or ✗.
  - Local actions and folders just open.
- **Checking the wiring:** with the board on USB, send `p` in a serial
  monitor. It prints `shortcut(17)=1` normally and `=0` while the button is
  held. Every press also logs `[Input] shortcut button pressed (GPIO17)`.
