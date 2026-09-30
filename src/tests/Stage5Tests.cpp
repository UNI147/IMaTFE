#include "Stage5Tests.h"

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
        if (value) { ++passed; TestLog::instance() << "  [PASS] " << name << '\n'; }
        else { ++failed; TestLog::instance() << "  [FAIL] " << name << '\n'; }
    }
};

using GPU = imatfe::gpu::GPU;
using Word = imatfe::core::u32;
using Half = imatfe::core::u16;

constexpr Word xy(int x, int y)
{
    return (static_cast<Word>(static_cast<imatfe::core::u16>(y)) << 16) |
           static_cast<Word>(static_cast<imatfe::core::u16>(x));
}
constexpr Half rgb15(int r, int g, int b)
{
    return static_cast<Half>((r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10));
}
void unrestricted(GPU& gpu)
{
    gpu.write_gp0(0xE3000000u);
    gpu.write_gp0(0xE403FFFFu);
}

void test_framebuffer(Runner& t)
{
    GPU gpu;
    t.check(gpu.vram().size() == GPU::VRAM_WIDTH * GPU::VRAM_HEIGHT,
            "VRAM contains 1024x512 16-bit pixels");
    t.check(gpu.vram(0, 0) == 0 && gpu.vram(1023, 511) == 0, "VRAM starts cleared");
    constexpr Half pattern[] = {0x0000u, 0x0001u, 0x7FFFu, 0x8000u, 0xFFFFu};
    for (std::size_t i = 0; i < std::size(pattern); ++i) {
        gpu.set_vram(i, 0, pattern[i]);
        t.check(gpu.vram(i, 0) == pattern[i], "16-bit pixel round-trip " + std::to_string(i));
    }
    gpu.set_vram(1023, 511, 0xA55Au);
    t.check(gpu.vram(1023, 511) == 0xA55Au, "Last VRAM pixel is addressable");
    gpu.set_vram(1024, 0, 0x1234u);
    gpu.set_vram(0, 512, 0x1234u);
    t.check(gpu.vram(1024, 0) == 0 && gpu.vram(0, 512) == 0, "Out-of-range access is safe");
    gpu.clear_vram(0xBEEFu);
    t.check(gpu.vram(0, 0) == 0xBEEFu && gpu.vram(1023, 511) == 0xBEEFu,
            "clear_vram fills the framebuffer");
}

void test_triangles(Runner& t)
{
    GPU gpu;
    unrestricted(gpu);
    gpu.write_gp0(0x200000FFu); // flat red triangle
    gpu.write_gp0(xy(10, 10)); gpu.write_gp0(xy(20, 10)); gpu.write_gp0(xy(10, 20));
    t.check(gpu.vram(11, 11) == rgb15(255, 0, 0), "Triangle fills an interior pixel");
    t.check(gpu.vram(19, 19) == 0, "Triangle rejects a pixel outside its coverage");

    gpu.clear_vram();
    gpu.write_gp0(0x200000FFu);
    gpu.write_gp0(xy(10, 20)); gpu.write_gp0(xy(20, 10)); gpu.write_gp0(xy(10, 10));
    t.check(gpu.vram(11, 11) == rgb15(255, 0, 0), "Triangle accepts reversed winding");

    gpu.clear_vram();
    gpu.write_gp0(0x200000FFu);
    gpu.write_gp0(xy(4, 4)); gpu.write_gp0(xy(4, 4)); gpu.write_gp0(xy(4, 4));
    t.check(gpu.vram(4, 4) == 0, "Degenerate triangle draws no pixels");
}

void test_rectangles_and_sprites(Runner& t)
{
    GPU gpu;
    unrestricted(gpu);
    gpu.write_gp0(0x600000FFu); gpu.write_gp0(xy(20, 30)); gpu.write_gp0(0x00030004u);
    const Half red = rgb15(255, 0, 0);
    t.check(gpu.vram(20, 30) == red && gpu.vram(23, 32) == red, "Variable rectangle fills width x height");
    t.check(gpu.vram(24, 32) == 0, "Rectangle right/bottom edge is exclusive");

    gpu.write_gp0(0x7000FF00u); gpu.write_gp0(xy(40, 40));
    t.check(gpu.vram(40, 40) == rgb15(0, 255, 0) && gpu.vram(47, 47) == rgb15(0, 255, 0),
            "8x8 fixed-size sprite rasterizes");
    t.check(gpu.vram(48, 47) == 0, "8x8 sprite has exclusive extent");
    gpu.write_gp0(0x780000FFu); gpu.write_gp0(xy(60, 60));
    t.check(gpu.vram(75, 75) == red && gpu.vram(76, 75) == 0, "16x16 fixed-size sprite rasterizes");
}

void test_clipping(Runner& t)
{
    GPU gpu;
    gpu.write_gp0(0xE3001805u); // drawing area [5,7] x [6,8]
    gpu.write_gp0(0xE4002007u);
    gpu.write_gp0(0x600000FFu); gpu.write_gp0(xy(3, 4)); gpu.write_gp0(0x00060006u);
    const Half red = rgb15(255, 0, 0);
    t.check(gpu.vram(5, 6) == red && gpu.vram(7, 8) == red, "Primitive is clipped to drawing area");
    t.check(gpu.vram(4, 6) == 0 && gpu.vram(8, 8) == 0, "Pixels outside drawing area remain untouched");

    // GP0(E5h) is applied before both drawing-area and physical VRAM clipping.
    gpu.clear_vram();
    gpu.write_gp0(0xE3000000u);
    gpu.write_gp0(0xE403FFFFu);
    gpu.write_gp0(0xE5000001u); // +1 X offset
    gpu.write_gp0(0x600000FFu); gpu.write_gp0(xy(1022, 10)); gpu.write_gp0(0x00040001u);
    t.check(gpu.vram(1023, 10) == red, "Offset is applied before VRAM clipping");
    t.check(gpu.vram(0, 10) == 0, "Offset primitive does not wrap across VRAM row");

    gpu.clear_vram();
    gpu.write_gp0(0xE3000000u);
    gpu.write_gp0(0xE4000000u); // only (0,0) is drawable
    gpu.write_gp0(0xE5000000u);
    gpu.write_gp0(0x600000FFu); gpu.write_gp0(xy(-1, 0)); gpu.write_gp0(0x00030001u);
    t.check(gpu.vram(0, 0) == 0, "Negative primitive coordinates are clipped, not wrapped");

    gpu.write_gp0(0xE300000Au); // left=10
    gpu.write_gp0(0xE4000005u); // right=5: empty/inverted area
    gpu.write_gp0(0x400000FFu); gpu.write_gp0(xy(0, 0)); gpu.write_gp0(xy(20, 0));
    t.check(gpu.vram(0, 0) == 0, "Inverted drawing area rejects line pixels");
}
}

bool run_stage5_tests()
{
    Runner t;
    TestLog::instance() << "\n=== Stage 5: Basic rasterization ===\n";
    test_framebuffer(t);
    test_triangles(t);
    test_rectangles_and_sprites(t);
    test_clipping(t);
    TestLog::instance() << "Stage 5 result: " << t.passed << " passed, " << t.failed << " failed.\n";
    return t.failed == 0;
}
} // namespace imatfe::tests
