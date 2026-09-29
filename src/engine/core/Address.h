#pragma once

#include "Types.h"

namespace imatfe::core::psx
{
constexpr Address KUSEG_BASE = 0x00000000u;
constexpr Address KSEG0_BASE = 0x80000000u;
constexpr Address KSEG1_BASE = 0xA0000000u;
constexpr Address KSEG2_BASE = 0xC0000000u;

constexpr Address MAIN_RAM_BASE       = 0x00000000u;
constexpr Address DEV0_BASE           = 0x1F000000u;
constexpr Address SCRATCHPAD_BASE     = 0x1F800000u;
constexpr Address IO_BASE             = 0x1F801000u;
constexpr Address DEV8_BASE           = 0x1F802000u;
constexpr Address DEV1_BASE           = 0x1FA00000u;
constexpr Address BIOS_BASE            = 0x1FC00000u;
constexpr Address CACHE_CONTROL_BASE  = 0xFFFE0000u;

constexpr std::size_t MAIN_RAM_SIZE      = MiB(2);
constexpr std::size_t SCRATCHPAD_SIZE    = KiB(1);
constexpr std::size_t IO_SIZE            = KiB(4);
constexpr std::size_t DEV8_SIZE          = KiB(8);
constexpr std::size_t DEV0_SIZE          = MiB(8);
constexpr std::size_t DEV1_SIZE          = MiB(2);
constexpr std::size_t BIOS_MAX_SIZE      = KiB(512);

constexpr bool is_kuseg(Address a) noexcept { return a < KSEG0_BASE; }
constexpr bool is_kseg0(Address a) noexcept { return a >= KSEG0_BASE && a < KSEG1_BASE; }
constexpr bool is_kseg1(Address a) noexcept { return a >= KSEG1_BASE && a < KSEG2_BASE; }
constexpr bool is_kseg2(Address a) noexcept { return a >= KSEG2_BASE; }

// KUSEG/KSEG0/KSEG1 all address the same 512 MiB physical bus window.
// The PSX only implements a small portion of that window; unmapped physical
// addresses remain unmapped instead of being silently turned into host RAM.
constexpr PhysicalAddress translate_segment(Address address) noexcept
{
    return address & 0x1FFFFFFFu;
}

constexpr bool is_aligned(Address address, std::size_t alignment) noexcept
{
    return (address & static_cast<Address>(alignment - 1)) == 0;
}

} // namespace imatfe::core::psx
