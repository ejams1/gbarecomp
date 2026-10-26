#include "gamepad_input.h"

#include <cstdlib>
#include <iostream>

namespace {
void check(const char* name, std::uint16_t actual, std::uint16_t expected) {
    if (actual == expected) return;
    std::cerr << name << ": expected KEYINPUT 0x" << std::hex << expected
              << ", got 0x" << actual << '\n';
    std::exit(1);
}
}

int main() {
    using recomp::input::gba_keyinput_from_gamepad;
    using recomp::input::kGamepadAxisDeadzone;

    check("released", gba_keyinput_from_gamepad(0, 0, 0), 0x03FF);
    check("A and L", gba_keyinput_from_gamepad((1u << 0) | (1u << 9), 0, 0),
          static_cast<std::uint16_t>(0x03FF & ~(1u << 0) & ~(1u << 9)));
    check("right axis", gba_keyinput_from_gamepad(0, kGamepadAxisDeadzone, 0),
          static_cast<std::uint16_t>(0x03FF & ~(1u << 4)));
    check("left axis", gba_keyinput_from_gamepad(0, -kGamepadAxisDeadzone, 0),
          static_cast<std::uint16_t>(0x03FF & ~(1u << 5)));
    check("up axis", gba_keyinput_from_gamepad(0, 0, -kGamepadAxisDeadzone),
          static_cast<std::uint16_t>(0x03FF & ~(1u << 6)));
    check("down axis", gba_keyinput_from_gamepad(0, 0, kGamepadAxisDeadzone),
          static_cast<std::uint16_t>(0x03FF & ~(1u << 7)));
    check("inside deadzone", gba_keyinput_from_gamepad(0,
          kGamepadAxisDeadzone - 1, -kGamepadAxisDeadzone + 1), 0x03FF);
    check("D-pad plus stick", gba_keyinput_from_gamepad(1u << 0,
          -kGamepadAxisDeadzone, kGamepadAxisDeadzone),
          static_cast<std::uint16_t>(0x03FF & ~(1u << 0) & ~(1u << 5) & ~(1u << 7)));
}
