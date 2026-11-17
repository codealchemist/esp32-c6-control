#pragma once

// KY-040 rotary encoder + single dedicated button reading.
// Pins are placeholders — see pins.h, MUST be confirmed before this does
// anything meaningful on real hardware.

enum class InputEvent {
  None,
  EncoderCW,
  EncoderCCW,
  EncoderPressShort,
  EncoderPressLong,
  ButtonPressShort,
  ButtonPressLong,
};

void inputInit();

// Call every loop() iteration. Returns at most one event per call (a
// simple depth-1 queue is enough for this POC's linear sequencing — the
// real app's input_manager, ARCHITECTURE.md §6.2, would need a real queue).
InputEvent inputPoll();

// True if the dedicated button is held down right now — sampled once at
// boot to decide whether to force re-provisioning (docs/POC.md §3).
bool inputButtonHeldNow();
