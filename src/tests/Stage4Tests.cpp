#include "Stage4Tests.h"

#include "TestLog.h"
#include "engine/gpu/GPU.h"

#include <string>

namespace imatfe::tests
{
namespace
{
struct Runner
{
    int passed = 0;
    int failed = 0;

    void check(bool value, const std::string& name)
    {
        if (value)
        {
            ++passed;
            TestLog::instance() << "  [PASS] " << name << '\n';
        }
        else
        {
            ++failed;
            TestLog::instance() << "  [FAIL] " << name << '\n';
        }
    }
};

using GPU = imatfe::gpu::GPU;
using Word = imatfe::core::u32;
using Half = imatfe::core::u16;

constexpr Word xy(int x, int y)
{
    return (static_cast<Word>(static_cast<core::u16>(y)) << 16) |
           static_cast<Word>(static_cast<core::u16>(x));
}

constexpr Half bgr15(int r, int g, int b)
{
    return static_cast<Half>((r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10));
}

void test_gp1_reset_and_state(Runner& t)
{
    GPU gpu;
    gpu.write_gp1(0x03000001u); // display off
    t.check(!gpu.display_enabled(), "GP1(03h) disables display");

    gpu.write_gp1(0x00000000u);
    t.check(gpu.display_enabled(), "GP1(00h) resets display state");
    t.check(gpu.read_gp1() == 0x14802000u, "GPUSTAT reset value");
    t.check(gpu.draw_area_left() == 0 && gpu.draw_area_top() == 0,
            "Drawing area top-left reset");
    t.check(gpu.draw_area_right() == 1023 && gpu.draw_area_bottom() == 511,
            "Drawing area bottom-right reset");
}

void test_gp0_environment(Runner& t)
{
    GPU gpu;
    gpu.write_gp0(0xE300140Au); // left=10, top=5
    gpu.write_gp0(0xE400A01Eu); // right=30, bottom=40
    t.check(gpu.draw_area_left() == 10 && gpu.draw_area_top() == 5,
            "GP0(E3h) sets drawing area origin");
    t.check(gpu.draw_area_right() == 30 && gpu.draw_area_bottom() == 40,
            "GP0(E4h) sets drawing area extent");

    gpu.write_gp0(0xE6000003u);
    gpu.write_gp0(0x6000FF00u);
    gpu.write_gp0(xy(10, 5));
    gpu.write_gp0(0x00010001u);
    t.check((gpu.vram(10, 5) & 0x8000u) != 0, "GP0(E6h) mask-set affects drawn pixel");
}

void test_rectangle(Runner& t)
{
    GPU gpu;
    gpu.write_gp0(0xE3000000u);
    gpu.write_gp0(0xE403FFFFu);
    gpu.write_gp0(0x600000FFu); // red
    gpu.write_gp0(xy(20, 30));
    gpu.write_gp0(0x00030004u); // 4x3

    const Half red = bgr15(255, 0, 0);
    t.check(gpu.vram(20, 30) == red, "Rectangle writes first pixel");
    t.check(gpu.vram(23, 32) == red, "Rectangle writes last pixel");
    t.check(gpu.vram(24, 32) == 0, "Rectangle uses exclusive right/bottom edge");
}

void test_triangle(Runner& t)
{
    GPU gpu;
    gpu.write_gp0(0xE3000000u);
    gpu.write_gp0(0xE403FFFFu);
    gpu.write_gp0(0x2000FF00u); // green flat triangle
    gpu.write_gp0(xy(10, 10));
    gpu.write_gp0(xy(20, 10));
    gpu.write_gp0(xy(10, 20));

    const Half green = bgr15(0, 255, 0);
    t.check(gpu.vram(11, 11) == green, "Triangle rasterizer fills interior");
    t.check(gpu.vram(19, 19) == 0, "Triangle rasterizer rejects outside pixel");
}

void test_line(Runner& t)
{
    GPU gpu;
    gpu.write_gp0(0x400000FFu); // red line
    gpu.write_gp0(xy(5, 5));
    gpu.write_gp0(xy(9, 9));
    const Half red = bgr15(255, 0, 0);
    t.check(gpu.vram(5, 5) == red && gpu.vram(9, 9) == red,
            "Line primitive includes both endpoints");
}

void test_quick_fill(Runner& t)
{
    GPU gpu;
    gpu.write_gp0(0x020000FFu);
    gpu.write_gp0(xy(0, 0));
    gpu.write_gp0(0x00010001u); // 16x1 in quick-fill units
    const Half red = bgr15(255, 0, 0);
    t.check(gpu.vram(0, 0) == red && gpu.vram(15, 0) == red,
            "GP0(02h) quick fill writes a 16-pixel span");
}

} // namespace

bool run_stage4_tests()
{
    Runner t;
    TestLog::instance() << "\n=== Stage 4: GPU ===\n";
    TestLog::instance() << "GP0/GP1 / VRAM / Environment / Basic Primitives\n";
    test_gp1_reset_and_state(t);
    test_gp0_environment(t);
    test_rectangle(t);
    test_triangle(t);
    test_line(t);
    test_quick_fill(t);
    TestLog::instance() << "Stage 4 result: " << t.passed << " passed, " << t.failed << " failed.\n";
    return t.failed == 0;
}

} // namespace imatfe::tests
