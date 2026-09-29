#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>

namespace imatfe::core
{
using u8  = std::uint8_t;
using s8  = std::int8_t;
using u16 = std::uint16_t;
using s16 = std::int16_t;
using u32 = std::uint32_t;
using s32 = std::int32_t;
using u64 = std::uint64_t;
using s64 = std::int64_t;

using Address = u32;
using PhysicalAddress = u32;

static_assert(sizeof(u8) == 1);
static_assert(sizeof(u16) == 2);
static_assert(sizeof(u32) == 4);
static_assert(sizeof(u64) == 8);

constexpr std::size_t KiB(std::size_t value) { return value * 1024u; }
constexpr std::size_t MiB(std::size_t value) { return value * 1024u * 1024u; }

} // namespace imatfe::core
