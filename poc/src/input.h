#pragma once

// KY-040 rotary encoder, the BOOT button and the shortcut button (pins.h).

enum class InputEvent {
  None,
  EncoderCW,
  EncoderCCW,
  EncoderPressShort,
  EncoderPressLong,
  ButtonPressShort,
  ButtonPressLong,
  // The second on-screen action button (UI rule: at most two per screen).
  // Touch-only; what it means is up to the screen showing it.
  SecondaryAction,
  // The shortcut button (PIN_SHORTCUT): runs the menu action assigned in
  // Settings > Shortcut button, from any menu page.
  ShortcutPress,
};

void inputInit();

// Call every loop() iteration. Returns at most one event per call (a
// simple depth-1 queue is enough for this POC's linear sequencing — the
// real app's input_manager, ARCHITECTURE.md §6.2, would need a real queue).
InputEvent inputPoll();

// Queues an event as if it came from the physical inputs — how on-screen
// touch buttons drive the same flow as the encoder/button. Next inputPoll()
// returns it ahead of hardware events. Call from the app task only (LVGL
// event callbacks run there).
void inputInject(InputEvent ev);

// True if the dedicated button is held down right now — sampled once at
// boot to decide whether to force re-provisioning (docs/POC.md §3).
bool inputButtonHeldNow();

// Prints the raw level of every input pin (serial key 'p'), for wiring checks.
void inputPrintPins();
