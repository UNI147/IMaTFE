#include "Stage1Tests.h"

#include "engine/core/Address.h"
#include "engine/core/Fixed.h"
#include "engine/core/Memory.h"
#include "engine/core/Types.h"

#include <cmath>
#include <functional>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace imatfe::tests
{
namespace
{
using imatfe::core::Fixed12;
using imatfe::core::psx::Memory;
using imatfe::core::psx::MemoryRegion;
using imatfe::core::psx::ResolvedAddress;
using imatfe::core::Address;

struct TestRunner
{
    int passed = 0;
    int failed = 0;

    void check(bool condition, const std::string& name)
    {
        if (condition)
        {
            ++passed;
            std::cout << "  [PASS] " << name << '\n';
        }
        else
        {
            ++failed;
            std::cout << "  [FAIL] " << name << '\n';
        }
    }

    template <typename Fn>
    void check_throws(Fn&& fn, const std::string& name)
    {
        bool threw = false;
        try
        {
            std::invoke(std::forward<Fn>(fn));
        }
        catch (...)
        {
            threw = true;
        }
        check(threw, name);
    }
};

bool same_resolution(const ResolvedAddress& actual,
                     MemoryRegion region,
                     std::size_t offset,
                     bool cached)
{
    return actual.region == region && actual.offset == offset && actual.cached == cached;
}

void test_types(TestRunner& t)
{
    std::cout << "\n[Basic Types]\n";
    t.check(sizeof(core::u8) == 1, "u8 is 8-bit");
    t.check(sizeof(core::s8) == 1, "s8 is 8-bit");
    t.check(sizeof(core::u16) == 2, "u16 is 16-bit");
    t.check(sizeof(core::s16) == 2, "s16 is 16-bit");
    t.check(sizeof(core::u32) == 4, "u32 is 32-bit");
    t.check(sizeof(core::s32) == 4, "s32 is 32-bit");
    t.check(sizeof(core::u64) == 8, "u64 is 64-bit");
    t.check(sizeof(core::s64) == 8, "s64 is 64-bit");
}

void test_address_segments(TestRunner& t)
{
    std::cout << "\n[Addressing / Segments]\n";

    t.check(core::psx::is_kuseg(0x00000000u), "KUSEG lower boundary");
    t.check(core::psx::is_kuseg(0x7FFFFFFFu), "KUSEG upper boundary");
    t.check(core::psx::is_kseg0(0x80000000u), "KSEG0 lower boundary");
    t.check(core::psx::is_kseg0(0x9FFFFFFFu), "KSEG0 upper boundary");
    t.check(core::psx::is_kseg1(0xA0000000u), "KSEG1 lower boundary");
    t.check(core::psx::is_kseg1(0xBFFFFFFFu), "KSEG1 upper boundary");
    t.check(core::psx::is_kseg2(0xC0000000u), "KSEG2 lower boundary");
    t.check(core::psx::is_kseg2(0xFFFFFFFFu), "KSEG2 upper boundary");

    t.check(core::psx::translate_segment(0x00001234u) == 0x00001234u,
            "KUSEG physical translation");
    t.check(core::psx::translate_segment(0x80001234u) == 0x00001234u,
            "KSEG0 physical translation");
    t.check(core::psx::translate_segment(0xA0001234u) == 0x00001234u,
            "KSEG1 physical translation");

    t.check(core::psx::is_aligned(0x1000u, 1), "1-byte alignment");
    t.check(core::psx::is_aligned(0x1000u, 2), "2-byte alignment");
    t.check(core::psx::is_aligned(0x1000u, 4), "4-byte alignment");
    t.check(!core::psx::is_aligned(0x1002u, 4), "misaligned 32-bit address is rejected");
}

void test_memory_resolution(TestRunner& t)
{
    std::cout << "\n[Memory Map / Resolution]\n";
    Memory memory;

    const auto check_region = [&t, &memory](Address address,
                                            MemoryRegion region,
                                            std::size_t offset,
                                            bool cached,
                                            const std::string& name)
    {
        t.check(same_resolution(memory.resolve(address), region, offset, cached), name);
    };

    check_region(0x00000000u, MemoryRegion::MainRam, 0, true, "Main RAM at 00000000h");
    check_region(0x80000000u, MemoryRegion::MainRam, 0, true, "Main RAM KSEG0 mirror");
    check_region(0xA0000000u, MemoryRegion::MainRam, 0, false, "Main RAM KSEG1 mirror");

    check_region(0x001FFFFCu, MemoryRegion::MainRam, 0x001FFFFCu, true,
                 "Main RAM last aligned 32-bit word");
    check_region(0x1F000000u, MemoryRegion::Dev0, 0, true, "DEV0 base");
    check_region(0x1F7FFFFCu, MemoryRegion::Dev0, 0x007FFFFCu, true, "DEV0 last aligned word");
    check_region(0x1F800000u, MemoryRegion::Scratchpad, 0, true, "Scratchpad base");
    check_region(0x1F8003FCu, MemoryRegion::Scratchpad, 0x3FC, true, "Scratchpad last aligned word");
    check_region(0x1F801000u, MemoryRegion::Io, 0, false, "I/O base");
    check_region(0x1F801FFCu, MemoryRegion::Io, 0xFFC, false, "I/O last aligned word");
    check_region(0x1F802000u, MemoryRegion::Dev8, 0, false, "DEV8 base");
    check_region(0x1F803FFCu, MemoryRegion::Dev8, 0x1FFCu, false, "DEV8 last aligned word");
    check_region(0x1FA00000u, MemoryRegion::Dev1, 0, true, "DEV1 base");
    check_region(0x1FBFFFFCu, MemoryRegion::Dev1, 0x001FFFFCu, true, "DEV1 last aligned word");
    check_region(0x1FC00000u, MemoryRegion::Bios, 0, false, "BIOS base");
    check_region(0x1FC7FFFCu, MemoryRegion::Bios, 0x7FFFC, false, "BIOS last aligned word");
    check_region(0xFFFE0000u, MemoryRegion::CacheControl, 0, false, "Cache control base");
    check_region(0xFFFE00FCu, MemoryRegion::CacheControl, 0xFC, false, "Cache control last byte");

    t.check(memory.resolve(0x1FC80000u).region == MemoryRegion::Unmapped,
            "Address immediately after default BIOS is unmapped");
    t.check(memory.resolve(0xC0000000u).region == MemoryRegion::Unmapped,
            "KSEG2 normal address is unmapped");
    t.check(memory.resolve(0xFFF00000u).region == MemoryRegion::Unmapped,
            "Unrelated KSEG2 address is unmapped");
}

void test_memory_data_path(TestRunner& t)
{
    std::cout << "\n[Memory Data Path]\n";
    Memory memory;

    memory.write32(0x00001000u, 0x12345678u);
    t.check(memory.read8(0x00001000u) == 0x78u, "Little-endian byte 0");
    t.check(memory.read8(0x00001001u) == 0x56u, "Little-endian byte 1");
    t.check(memory.read8(0x00001002u) == 0x34u, "Little-endian byte 2");
    t.check(memory.read8(0x00001003u) == 0x12u, "Little-endian byte 3");
    t.check(memory.read16(0x00001000u) == 0x5678u, "16-bit little-endian read");
    t.check(memory.read32(0x00001000u) == 0x12345678u, "32-bit round-trip");

    t.check(memory.read32(0x80001000u) == 0x12345678u, "KSEG0 sees KUSEG RAM data");
    t.check(memory.read32(0xA0001000u) == 0x12345678u, "KSEG1 sees KUSEG RAM data");

    memory.write16(0x1F800100u, 0xBEEFu);
    t.check(memory.read16(0x1F800100u) == 0xBEEFu, "Scratchpad 16-bit round-trip");

    memory.write8(0x1F801000u, 0x5Au);
    t.check(memory.read8(0x1F801000u) == 0x5Au, "I/O storage 8-bit round-trip");

    memory.write32(0x1F802000u, 0xCAFEBABEu);
    t.check(memory.read32(0x1F802000u) == 0xCAFEBABEu, "DEV8 32-bit round-trip");

    memory.write32(0x1FA00000u, 0x0BADF00Du);
    t.check(memory.read32(0x1FA00000u) == 0x0BADF00Du, "DEV1 32-bit round-trip");

    memory.write32(0xFFFE0000u, 0xA5A5A5A5u);
    t.check(memory.read32(0xFFFE0000u) == 0xA5A5A5A5u, "Cache-control storage round-trip");
}

void test_memory_errors(TestRunner& t)
{
    std::cout << "\n[Memory Error Conditions]\n";
    Memory memory;

    t.check_throws([&] { memory.read16(0x00001001u); }, "Unaligned 16-bit read throws");
    t.check_throws([&] { memory.read32(0x00001002u); }, "Unaligned 32-bit read throws");
    t.check_throws([&] { memory.write16(0x00001001u, 0x1234u); }, "Unaligned 16-bit write throws");
    t.check_throws([&] { memory.write32(0x00001002u, 0x12345678u); }, "Unaligned 32-bit write throws");

    t.check_throws([&] { memory.read32(0x1FC80000u); }, "Unmapped read throws");
    t.check_throws([&] { memory.write32(0x1FC80000u, 0); }, "Unmapped write throws");
    t.check_throws([&] { memory.read32(0xC0000000u); }, "KSEG2 read throws");
    t.check_throws([&] { memory.write32(0xC0000000u, 0); }, "KSEG2 write throws");
    t.check_throws([&] { memory.write32(0x1FC00000u, 0x12345678u); }, "BIOS write is rejected");

    t.check_throws([&] { Memory invalid_bios(core::psx::BIOS_MAX_SIZE + 1); }, "Oversized BIOS mapping is rejected");
}

void test_fixed_point(TestRunner& t)
{
    std::cout << "\n[Fixed-Point]\n";

    const auto zero = Fixed12::from_integer(0);
    const auto one = Fixed12::from_integer(1);
    const auto two = Fixed12::from_integer(2);
    const auto three = Fixed12::from_integer(3);
    const auto one_half = Fixed12::from_float(1.5);
    const auto quarter = Fixed12::from_float(0.25);

    t.check(zero.raw() == 0, "Fixed12 zero raw value");
    t.check(one.raw() == (1 << 12), "Fixed12 integer scaling");
    t.check(std::abs(one_half.to_double() - 1.5) < 1e-12, "Fixed12 float conversion");
    t.check((one + two).raw() == three.raw(), "Fixed12 addition");
    t.check((three - one).raw() == two.raw(), "Fixed12 subtraction");
    t.check((-one).raw() == -one.raw(), "Fixed12 unary minus");
    t.check((one_half * two).raw() == three.raw(), "Fixed12 multiplication");
    t.check((three / two).raw() == Fixed12::from_float(1.5).raw(), "Fixed12 division");
    t.check((one_half + quarter).raw() == Fixed12::from_float(1.75).raw(), "Fixed12 fractional addition");
    t.check(one < two, "Fixed12 ordering");
    t.check(three > two, "Fixed12 ordering (greater-than)");
    t.check(one != two, "Fixed12 inequality");

    t.check(Fixed12::from_raw(1234).raw() == 1234, "Fixed12 raw constructor round-trip");
}

} // namespace

bool run_stage1_tests()
{
    std::cout << "\n=== IMaTFE Stage 1 Verification ===\n";
    std::cout << "Basic Types / Fixed-Point / Addressing / Memory Model\n";

    TestRunner runner;
    test_types(runner);
    test_address_segments(runner);
    test_memory_resolution(runner);
    test_memory_data_path(runner);
    test_memory_errors(runner);
    test_fixed_point(runner);

    std::cout << "\n=== Stage 1 Test Summary ===\n";
    std::cout << "Passed: " << runner.passed << '\n';
    std::cout << "Failed: " << runner.failed << '\n';

    return runner.failed == 0;
}

} // namespace imatfe::tests
