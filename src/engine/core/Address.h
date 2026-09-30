#pragma once

#include "Types.h"

#include <cstddef>

namespace imatfe::core::psx {

using Address = imatfe::core::u32;
using PhysicalAddress = imatfe::core::u32;

constexpr std::size_t MAIN_RAM_SIZE = 0x00200000u;
constexpr std::size_t DEV0_SIZE = 0x00800000u;
constexpr std::size_t SCRATCHPAD_SIZE = 0x00000400u;
constexpr std::size_t IO_SIZE = 0x00001000u;
constexpr std::size_t DEV8_SIZE = 0x00010000u;
constexpr std::size_t DEV1_SIZE = 0x00200000u;
constexpr std::size_t BIOS_MAX_SIZE = 0x00080000u;

constexpr PhysicalAddress MAIN_RAM_BASE = 0x00000000u;
constexpr PhysicalAddress DEV0_BASE = 0x1F000000u;
constexpr PhysicalAddress SCRATCHPAD_BASE = 0x1F800000u;
constexpr PhysicalAddress IO_BASE = 0x1F801000u;
constexpr PhysicalAddress DEV8_BASE = 0x1F802000u;
constexpr PhysicalAddress DEV1_BASE = 0x1FA00000u;
constexpr PhysicalAddress BIOS_BASE = 0x1FC00000u;
constexpr PhysicalAddress CACHE_CONTROL_BASE = 0xFFFE0000u;

constexpr bool is_kuseg(Address a) {
    return a < 0x80000000u;
}

constexpr bool is_kseg0(Address a) {
    return a >= 0x80000000u && a < 0xA0000000u;
}

constexpr bool is_kseg1(Address a) {
    return a >= 0xA0000000u && a < 0xC0000000u;
}

constexpr bool is_kseg2(Address a) {
    return a >= 0xC0000000u;
}

constexpr PhysicalAddress translate_segment(Address a) {
    if (is_kuseg(a)) return a;
    if (is_kseg0(a)) return a - 0x80000000u;
    if (is_kseg1(a)) return a - 0xA0000000u;
    return a - 0xA0000000u;
}

constexpr bool is_aligned(Address a, std::size_t n) {
    return n != 0 && (a & (n - 1)) == 0;
}

}