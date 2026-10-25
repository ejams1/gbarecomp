#include <cstdint>

// M0-3 window-only host. Cartridge execution remains disabled until M0-4
// supplies the generated dispatch_table.cpp from the verified fork ROM.
struct DispatchEntry {
    std::uint32_t addr;
    std::uint8_t thumb;
    std::uint8_t resume;
    void (*fn)(void);
};

extern "C" const DispatchEntry kDispatchTable[] = {
    {0u, 0u, 0u, nullptr},
};
extern "C" const unsigned kDispatchTableLen = 0;

extern "C" const DispatchEntry kBiosDispatchTable[] = {
    {0u, 0u, 0u, nullptr},
};
extern "C" const unsigned kBiosDispatchTableLen = 0;
