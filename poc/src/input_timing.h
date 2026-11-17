#pragma once

#include <cstdint>

// POC-scoped copy of docs/ARCHITECTURE.md §6.2.1's timing constants —
// starting defaults, expected to be retuned after hands-on testing.
namespace input_timing {

constexpr uint32_t kButtonLongPressMs = 600;
constexpr uint32_t kEncoderLongPressMs = 600;
constexpr uint32_t kButtonDebounceMs = 30;
constexpr uint32_t kEncoderDebounceMs = 30;

} // namespace input_timing
