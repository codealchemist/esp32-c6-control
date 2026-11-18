#include "input.h"

#include "display.h"

#include <Arduino.h>

#include "input_timing.h"
#include "pins.h"

namespace {

// Standard 4-state ("Buxton-style") full-step quadrature decode table —
// a widely-used, well-tested pattern for KY-040-class encoders. Mechanical
// bounce/timing on your specific unit may still need the debounce constants
// in input_timing.h tuned; this table alone won't fix a unit with unusually
// bad contacts.
constexpr uint8_t R_START = 0x0;
constexpr uint8_t R_CW_FINAL = 0x1;
constexpr uint8_t R_CW_BEGIN = 0x2;
constexpr uint8_t R_CW_NEXT = 0x3;
constexpr uint8_t R_CCW_BEGIN = 0x4;
constexpr uint8_t R_CCW_FINAL = 0x5;
constexpr uint8_t R_CCW_NEXT = 0x6;
constexpr uint8_t DIR_CW = 0x10;
constexpr uint8_t DIR_CCW = 0x20;

const uint8_t kTransitionTable[7][4] = {
    {R_START, R_CW_BEGIN, R_CCW_BEGIN, R_START},
    {R_CW_NEXT, R_START, R_CW_FINAL, uint8_t(R_START | DIR_CW)},
    {R_CW_NEXT, R_CW_BEGIN, R_START, R_START},
    {R_CW_NEXT, R_CW_BEGIN, R_CW_FINAL, R_START},
    {R_CCW_NEXT, R_START, R_CCW_BEGIN, R_START},
    {R_CCW_NEXT, R_CCW_FINAL, R_START, uint8_t(R_START | DIR_CCW)},
    {R_CCW_NEXT, R_CCW_FINAL, R_CCW_BEGIN, R_START},
};

volatile uint8_t gEncState = R_START;
volatile int32_t gEncTicks = 0; // +1 per CW detent, -1 per CCW detent

void IRAM_ATTR onEncoderPinChange() {
  const uint8_t pinState =
      (digitalRead(PIN_ENCODER_CLK) << 1) | digitalRead(PIN_ENCODER_DT);
  gEncState = kTransitionTable[gEncState & 0x0F][pinState];
  const uint8_t dir = gEncState & 0x30;
  if (dir == DIR_CW) {
    gEncTicks++;
  } else if (dir == DIR_CCW) {
    gEncTicks--;
  }
}

struct ButtonState {
  uint8_t pin;
  bool lastStable = true; // pull-up idle-high assumed
  bool lastRaw = true;
  uint32_t lastChangeMs = 0;
  uint32_t pressStartMs = 0;
  bool pressed = false;
  bool longFired = false;
};

ButtonState gButton;
ButtonState gEncoderButton;

// Touch-injected events. Small ring buffer: a quick double-tap on "+" must
// count twice, which a single slot would drop.
constexpr uint8_t kInjectCap = 8;
InputEvent gInjected[kInjectCap];
uint8_t gInjectHead = 0, gInjectTail = 0;

// Returns Short/Long press event on release, None otherwise. Long-press
// fires once, on crossing the threshold, without waiting for release —
// matches "long press = Home" needing to feel immediate, not release-gated.
InputEvent pollButton(ButtonState &b, uint32_t debounceMs, uint32_t longPressMs,
                       InputEvent shortEvt, InputEvent longEvt) {
  const bool raw = digitalRead(b.pin) == HIGH; // idle-high, pressed = LOW
  const uint32_t now = millis();

  if (raw != b.lastRaw) {
    b.lastChangeMs = now;
    b.lastRaw = raw;
  }

  if ((now - b.lastChangeMs) >= debounceMs && b.lastStable != b.lastRaw) {
    b.lastStable = b.lastRaw;
    if (!b.lastStable) {
      // just pressed (active low)
      b.pressed = true;
      b.pressStartMs = now;
      b.longFired = false;
    } else {
      // just released
      if (b.pressed && !b.longFired) {
        b.pressed = false;
        return shortEvt;
      }
      b.pressed = false;
    }
  }

  if (b.pressed && !b.longFired && (now - b.pressStartMs) >= longPressMs) {
    b.longFired = true;
    return longEvt;
  }

  return InputEvent::None;
}

} // namespace

void inputInit() {
  pinMode(PIN_BUTTON, INPUT_PULLUP);
  pinMode(PIN_ENCODER_SW, INPUT_PULLUP);
  pinMode(PIN_ENCODER_CLK, INPUT_PULLUP);
  pinMode(PIN_ENCODER_DT, INPUT_PULLUP);

  gButton.pin = PIN_BUTTON;
  gEncoderButton.pin = PIN_ENCODER_SW;

  attachInterrupt(digitalPinToInterrupt(PIN_ENCODER_CLK), onEncoderPinChange, CHANGE);
  attachInterrupt(digitalPinToInterrupt(PIN_ENCODER_DT), onEncoderPinChange, CHANGE);
}

void inputInject(InputEvent ev) {
  const uint8_t next = (gInjectTail + 1) % kInjectCap;
  if (next == gInjectHead) return;  // full — drop rather than overwrite
  gInjected[gInjectTail] = ev;
  gInjectTail = next;
}

static InputEvent inputPollRaw();

// Every input restarts the auto-dim timer; one that wakes a dimmed screen is
// consumed by the wake (display.h).
InputEvent inputPoll() {
  const InputEvent ev = inputPollRaw();
  if (ev != InputEvent::None && displayNoteActivity()) return InputEvent::None;
  return ev;
}

static InputEvent inputPollRaw() {
  if (gInjectHead != gInjectTail) {
    const InputEvent ev = gInjected[gInjectHead];
    gInjectHead = (gInjectHead + 1) % kInjectCap;
    return ev;
  }

  // Encoder rotation takes priority if a tick is pending — rotation and
  // button events aren't expected to matter in the same instant.
  noInterrupts();
  int32_t ticks = gEncTicks;
  gEncTicks = 0;
  interrupts();

  if (ticks > 0) return InputEvent::EncoderCW;
  if (ticks < 0) return InputEvent::EncoderCCW;

  InputEvent ev = pollButton(gEncoderButton, input_timing::kEncoderDebounceMs,
                              input_timing::kEncoderLongPressMs,
                              InputEvent::EncoderPressShort, InputEvent::EncoderPressLong);
  if (ev != InputEvent::None) return ev;

  return pollButton(gButton, input_timing::kButtonDebounceMs,
                     input_timing::kButtonLongPressMs, InputEvent::ButtonPressShort,
                     InputEvent::ButtonPressLong);
}

bool inputButtonHeldNow() { return digitalRead(PIN_BUTTON) == LOW; }
