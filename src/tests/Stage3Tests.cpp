#include "Stage3Tests.h"

#include "engine/core/Memory.h"
#include "engine/cpu/CPU.h"
#include "engine/gte/GTE.h"

#include <iostream>
#include <string>

namespace imatfe::tests
{
namespace
{
struct Runner
{
    int passed = 0;
    int failed = 0;
    void check(bool v, const std::string& name)
    {
        if (v) { ++passed; std::cout << "  [PASS] " << name << '\n'; }
        else { ++failed; std::cout << "  [FAIL] " << name << '\n'; }
    }
};

using GTE = imatfe::gte::GTE;
using CPU = imatfe::cpu::CPU;
using Memory = imatfe::core::psx::Memory;
using Word = imatfe::core::u32;

constexpr Word cop2(Word rs, Word rt, Word rd)
{
    return (0x12u << 26) | (rs << 21) | (rt << 16) | (rd << 11);
}
constexpr Word cmd(Word opcode, Word sf = 1, Word mx = 0, Word v = 0, Word cv = 0, Word lm = 0)
{
    return (0x12u << 26) | (0x10u << 21) |
           (sf << 19) | (mx << 17) | (v << 15) | (cv << 13) |
           (lm << 10) | opcode;
}
constexpr Word pack16(Word lo, Word hi)
{
    return (lo & 0xFFFFu) | ((hi & 0xFFFFu) << 16);
}

void set_identity_rt(GTE& g)
{
    g.write_control(0, pack16(0x1000, 0));
    g.write_control(1, 0);
    g.write_control(2, pack16(0x1000, 0));
    g.write_control(3, pack16(0, 0));
    g.write_control(4, 0x1000);
}

void setup_projection(GTE& g)
{
    set_identity_rt(g);
    g.write_control(5, 0);
    g.write_control(6, 0);
    g.write_control(7, 0);
    g.write_control(24, 320u << 16);
    g.write_control(25, 240u << 16);
    g.write_control(26, 256);
    g.write_control(27, 0);
    g.write_control(28, 0);
}

void test_register_file(Runner& t)
{
    GTE g;
    g.write_data(0, pack16(10, static_cast<Word>(static_cast<core::u16>(-20))));
    g.write_data(1, static_cast<Word>(30));
    t.check(static_cast<core::s16>(g.read_data(0)) == 10, "GTE VXY0 low halfword");
    t.check(static_cast<core::s16>(g.read_data(0) >> 16) == -20, "GTE VXY0 high halfword");
    t.check(static_cast<core::s16>(g.read_data(1)) == 30, "GTE VZ0 signed halfword");

    g.write_data(12, pack16(1, 2));
    g.write_data(13, pack16(3, 4));
    g.write_data(14, pack16(5, 6));
    g.write_data(15, pack16(7, 8)); // SXYP pushes
    t.check(g.read_data(12) == pack16(3, 4), "SXYP write shifts SXY0");
    t.check(g.read_data(13) == pack16(5, 6), "SXYP write shifts SXY1");
    t.check(g.read_data(14) == pack16(7, 8), "SXYP write sets SXY2");
    t.check(g.read_data(15) == g.read_data(14), "SXYP read mirrors SXY2");
}

void test_mvmva(Runner& t)
{
    GTE g;
    set_identity_rt(g);
    g.write_control(5, 100);
    g.write_control(6, static_cast<Word>(static_cast<core::s32>(-50)));
    g.write_control(7, 200);
    g.write_data(0, pack16(2 << 12, 3 << 12));
    g.write_data(1, 4 << 12);

    g.execute(cmd(0x12, 1, 0, 0, 0, 0));

    t.check(static_cast<core::s16>(g.read_data(9)) == 8292, "MVMVA X + translation");
    t.check(static_cast<core::s16>(g.read_data(10)) == 12238, "MVMVA Y + negative translation");
    t.check(static_cast<core::s16>(g.read_data(11)) == 16584, "MVMVA Z + translation");
}

void test_rtps_and_rtpt_fifo(Runner& t)
{
    GTE g;
    setup_projection(g);
    g.write_data(0, pack16(10, 20)); g.write_data(1, 100);
    g.execute(cmd(0x01));

    const Word sxy2 = g.read_data(14);
    const core::s16 sx = static_cast<core::s16>(sxy2 & 0xFFFFu);
    const core::s16 sy = static_cast<core::s16>(sxy2 >> 16);
    t.check(sx == 339, "RTPS produces projected X");
    t.check(sy == 279, "RTPS produces projected Y");
    t.check(g.read_data(19) == 100, "RTPS updates SZ3");
    t.check(g.read_data(12) == 0 && g.read_data(13) == 0, "Initial SXY FIFO state");

    g.reset();
    setup_projection(g);
    g.write_data(0, pack16(10, 20)); g.write_data(1, 100);
    g.write_data(2, pack16(20, 30)); g.write_data(3, 100);
    g.write_data(4, pack16(30, 40)); g.write_data(5, 100);
    g.execute(cmd(0x30));
    t.check(static_cast<core::s16>(g.read_data(12)) == 339 && static_cast<core::s16>(g.read_data(12) >> 16) == 279,
            "RTPT preserves V0 as SXY0");
    t.check(static_cast<core::s16>(g.read_data(13)) == 359 && static_cast<core::s16>(g.read_data(13) >> 16) == 299,
            "RTPT places V1 in SXY1");
    t.check(static_cast<core::s16>(g.read_data(14)) == 379 && static_cast<core::s16>(g.read_data(14) >> 16) == 319,
            "RTPT places V2 in SXY2");
    t.check(g.read_data(17) == 100 && g.read_data(18) == 100 && g.read_data(19) == 100,
            "RTPT shifts SZ FIFO for three vertices");
}

void test_math_commands(Runner& t)
{
    GTE g;
    g.write_data(12, pack16(0,0)); g.write_data(13, pack16(10,0)); g.write_data(14, pack16(0,10));
    g.execute(cmd(0x06));
    t.check(static_cast<core::s32>(g.read_data(24)) == 100, "NCLIP triangle area");

    for (int i = 0; i < 8; ++i) g.tick();
    g.write_control(0, 1); g.write_control(2, 1); g.write_control(4, 1);
    g.write_data(9, 1); g.write_data(10, 2); g.write_data(11, 3);
    g.execute(cmd(0x0C, 0, 0, 0, 0, 0));
    for (int i = 0; i < 6; ++i) g.tick();
    t.check(static_cast<core::s16>(g.read_data(9)) == 1 && static_cast<core::s16>(g.read_data(10)) == -2 && static_cast<core::s16>(g.read_data(11)) == 1,
            "OP cross product");

    g.write_data(9, 2 << 12);
    g.write_data(10, 3 << 12); g.write_data(11, 4 << 12);
    g.execute(cmd(0x28, 1, 0, 0, 0, 0));
    for (int i = 0; i < 5; ++i) g.tick();
    t.check(static_cast<core::s16>(g.read_data(9)) == 16384 && static_cast<core::s16>(g.read_data(10)) == 32767 && static_cast<core::s16>(g.read_data(11)) == 32767,
            "SQR with sf=1");

    g.write_data(16, 10); g.write_data(17, 20); g.write_data(18, 30); g.write_data(19, 40);
    g.write_control(29, 0x1000); g.write_control(30, 0x1000);
    g.execute(cmd(0x2D));
    t.check(g.read_data(7) == 90, "AVSZ3 computes scaled average");
    for (int i = 0; i < 5; ++i) g.tick();
    g.execute(cmd(0x2E));
    for (int i = 0; i < 6; ++i) g.tick();
    t.check(g.read_data(7) == 100, "AVSZ4 computes scaled average");

    g.write_data(8, 0x1000); g.write_data(9, 2); g.write_data(10, 3); g.write_data(11, 4);
    g.execute(cmd(0x3D, 1));
    for (int i = 0; i < 5; ++i) g.tick();
    t.check(g.read_data(25) == 2 && g.read_data(26) == 3 && g.read_data(27) == 4, "GPF interpolates vector");

    g.write_data(25, 0x1000); g.write_data(26, 0x2000); g.write_data(27, 0x3000);
    g.execute(cmd(0x3E, 1));
    for (int i = 0; i < 5; ++i) g.tick();
    t.check(g.read_data(25) == 4098, "GPL adds MAC base before SAR");
}

void test_flags_and_boundaries(Runner& t)
{
    GTE g;
    g.write_data(9, 0x7FFF); g.write_data(10, 0x7FFF); g.write_data(11, 0x7FFF);
    g.write_data(8, 0x7FFF);
    g.execute(cmd(0x3D, 1, 0, 0, 0, 1));
    t.check((g.flag() & (1u << 24)) != 0, "IR1 saturation flag is set");
    t.check((g.flag() & 0x80000000u) != 0, "FLAG error summary bit is set");

    g.reset();
    g.write_data(28, 0x7FFF);
    t.check(g.read_data(9) == 0x0F80 && g.read_data(10) == 0x0F80 && g.read_data(11) == 0x0F80,
            "IRGB expands 5:5:5 into 0..0x0F80");
    t.check((g.read_data(29) & 0x7FFFu) == 0x7FFF, "ORGB mirrors saturated IR values");

    g.reset();
    g.write_data(30, 0x00FFFFFFu);
    t.check(g.read_data(31) == 8, "LZCR counts leading zeroes");
    g.write_data(30, 0xFFF00000u);
    t.check(g.read_data(31) == 12, "LZCR counts leading ones");

    g.reset();
    g.write_control(31, 0xFFFFFFFFu);
    t.check((g.flag() & ~(0x80000000u | 0x7F87E000u)) == 0, "FLAG writes are masked to architectural bits");
}

void test_cpu_cop2_and_interlock(Runner& t)
{
    Memory memory;
    CPU cpu(memory);
    cpu.reset(0x80000000u);
    cpu.state().status |= (1u << 30);

    memory.write32(0, 0x2401000Au); // addiu r1,r0,10
    memory.write32(4, cop2(0x04, 1, 9)); // MTC2 r1,IR1
    memory.write32(8, cop2(0x00, 2, 9)); // MFC2 r2,IR1
    memory.write32(12, 0);

    cpu.step(); cpu.step(); cpu.step();
    t.check(cpu.gte().read_data(9) == 10, "CPU MTC2 reaches GTE");
    cpu.step();
    t.check(cpu.reg(2) == 10, "CPU MFC2 reads GTE through load-delay path");

    Memory timing_memory;
    CPU timing_cpu(timing_memory);
    timing_cpu.reset(0x80000000u);
    timing_cpu.state().status |= (1u << 30);
    // RTPS then an immediate MFC2. The MFC2 must remain at PC=4 until the
    // 15-cycle GTE operation has drained; the following NOP commits its load.
    timing_memory.write32(0, cmd(0x01));
    timing_memory.write32(4, cop2(0x00, 2, 14));
    timing_memory.write32(8, 0);
    setup_projection(timing_cpu.gte());
    timing_cpu.gte().write_data(0, pack16(10,20));
    timing_cpu.gte().write_data(1,100);

    timing_cpu.step();
    t.check(timing_cpu.gte().busy_cycles() == 14, "RTPS leaves 14 interlock cycles after issue");
    for (int i = 0; i < 14; ++i) timing_cpu.step();
    t.check(timing_cpu.state().pc == 0x80000004u, "MFC2 stays blocked until RTPS completes");
    timing_cpu.step();
    t.check(timing_cpu.state().pc == 0x80000008u, "MFC2 retires after GTE completion");
    timing_cpu.step();
    t.check(timing_cpu.reg(2) != 0, "MFC2 result follows normal CPU load delay");
}

} // namespace

bool run_stage3_tests()
{
    Runner t;
    std::cout << "\n=== Stage 3: GTE ===\n";
    test_register_file(t);
    test_mvmva(t);
    test_rtps_and_rtpt_fifo(t);
    test_math_commands(t);
    test_flags_and_boundaries(t);
    test_cpu_cop2_and_interlock(t);
    std::cout << "Stage 3 result: " << t.passed << " passed, " << t.failed << " failed.\n";
    return t.failed == 0;
}

} // namespace imatfe::tests
