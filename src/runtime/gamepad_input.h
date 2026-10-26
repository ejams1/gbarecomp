// gamepad_input.h — convert SDL's standard digital/left-stick state into GBA
// KEYINPUT bits. The keyboard path remains separate and additive.
#pragma once

#include <cstdint>

namespace recomp::input {

// `pressed_buttons` uses active-high GBA button bit positions. SDL's standard
// GameController layout is translated by the caller. Axis input supplements
// the D-pad; it never replaces it.
inline constexpr std::int16_t kGamepadAxisDeadzone = 16000;

inline std::uint16_t gba_keyinput_from_gamepad(
    std::uint16_t pressed_buttons, std::int16_t left_x,
    std::int16_t left_y) {
    constexpr std::uint16_t kRight = 1u << 4;
    constexpr std::uint16_t kLeft = 1u << 5;
    constexpr std::uint16_t kUp = 1u << 6;
    constexpr std::uint16_t kDown = 1u << 7;

    if (left_x >= kGamepadAxisDeadzone) pressed_buttons |= kRight;
    if (left_x <= -kGamepadAxisDeadzone) pressed_buttons |= kLeft;
    if (left_y <= -kGamepadAxisDeadzone) pressed_buttons |= kUp;
    if (left_y >= kGamepadAxisDeadzone) pressed_buttons |= kDown;

    return static_cast<std::uint16_t>(~pressed_buttons) & 0x03FFu;
}

}  // namespace recomp::input
