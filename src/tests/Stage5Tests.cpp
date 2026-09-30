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

void test_gouraud_shading(Runner& t)
{
    GPU gpu;
    unrestricted(gpu);

    // GP0(30h): Gouraud-shaded, untextured triangle.
    // The command contains vertex 0 color; colors 1 and 2 precede vertices 1 and 2.
    // The PSX GPU linearly interpolates the per-vertex colors across the polygon.
    // At pixel (11,11), sampled at its center (11.5,11.5), the barycentric weights
    // for triangle (10,10)-(20,10)-(10,20) are 0.70, 0.15, 0.15.
    const Word green = 0x00FF00u;
    const Word blue = 0xFF0000u;

    gpu.write_gp0(0x300000FFu);
    gpu.write_gp0(xy(10, 10));
    gpu.write_gp0(green);
    gpu.write_gp0(xy(20, 10));
    gpu.write_gp0(blue);
    gpu.write_gp0(xy(10, 20));

    t.check(gpu.vram(11, 11) == rgb15(179, 38, 38),
            "Gouraud triangle interpolates all three 8-bit color channels");
    t.check(gpu.vram(10, 15) != rgb15(255, 0, 0) &&
            gpu.vram(10, 15) != rgb15(0, 255, 0) &&
            gpu.vram(10, 15) != rgb15(0, 0, 255),
            "Gouraud interior pixels are not forced to a vertex color");

    // Gouraud shading is a polygon attribute; flat triangles keep the command color
    // over the complete primitive.
    gpu.clear_vram();
    gpu.write_gp0(0x200000FFu);
    gpu.write_gp0(xy(10, 10));
    gpu.write_gp0(xy(20, 10));
    gpu.write_gp0(xy(10, 20));
    t.check(gpu.vram(11, 11) == rgb15(255, 0, 0),
            "Flat triangle still uses one color across the primitive");

    // Quads are internally split as (1,2,3) and (2,3,4); each triangle keeps
    // the corresponding Gouraud vertex colors.
    gpu.clear_vram();
    gpu.write_gp0(0x380000FFu); // Gouraud quad
    gpu.write_gp0(xy(10, 10));
    gpu.write_gp0(green);
    gpu.write_gp0(xy(20, 10));
    gpu.write_gp0(blue);
    gpu.write_gp0(xy(20, 20));
    gpu.write_gp0(0x00FFFF00u); // yellow vertex 4
    gpu.write_gp0(xy(10, 20));
    t.check(gpu.vram(11, 18) != 0,
            "Gouraud quad rasterizes through its internal triangle split");
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


void set_tpage(GPU& gpu, Half tpage)
{
    gpu.write_gp0(0xE1000000u | static_cast<Word>(tpage));
}

void test_textured_rectangles(Runner& t)
{
    // 4-bit CLUT: four indices are packed into one VRAM halfword, low nibble first.
    {
        GPU gpu;
        const Half red = rgb15(255, 0, 0);
        const Half green = rgb15(0, 255, 0);
        const Half blue = rgb15(0, 0, 255);
        const Half white = rgb15(255, 255, 255);
        gpu.set_vram(0, 0, 0x4321u);
        gpu.set_vram(1, 0, 0x0005u); // U=4 crosses into the next packed halfword.
        gpu.set_vram(17, 0, red); gpu.set_vram(18, 0, green);
        gpu.set_vram(19, 0, blue); gpu.set_vram(20, 0, white);
        gpu.set_vram(21, 0, red);
        set_tpage(gpu, 0); // 4-bit, page origin = (0,0)
        gpu.write_gp0(0x65000000u); // raw textured variable rectangle
        gpu.write_gp0(xy(100, 20));
        gpu.write_gp0(0x00000000u | (1u << 16)); // CLUT x=16, y=0
        gpu.write_gp0(0x00010005u); // 5x1
        t.check(gpu.vram(100, 20) == red && gpu.vram(101, 20) == green &&
                gpu.vram(102, 20) == blue && gpu.vram(103, 20) == white &&
                gpu.vram(104, 20) == red,
                "4-bit texture extracts packed indices and crosses halfword boundary");
    }

    // Index 0 is a real palette index. It is not implicitly transparent.
    {
        GPU gpu;
        const Half yellow = rgb15(255, 255, 0);
        gpu.set_vram(0, 0, 0x0000u); // U=0 -> index 0.
        gpu.set_vram(16, 0, yellow);  // CLUT entry 0.
        set_tpage(gpu, 0);
        gpu.write_gp0(0x65000000u);
        gpu.write_gp0(xy(105, 20));
        gpu.write_gp0(0x00010000u); // CLUT x=16, y=0
        gpu.write_gp0(0x00010001u); // 1x1
        t.check(gpu.vram(105, 20) == yellow,
                "4-bit texture index 0 resolves through CLUT entry 0");
    }

    // CBA selects the palette row. X is in 16-halfword units, Y is a VRAM row.
    {
        GPU gpu;
        const Half cyan = rgb15(0, 255, 255);
        gpu.set_vram(0, 0, 0x0002u);
        gpu.set_vram(34, 7, cyan); // CBA: x=32, y=7; index 2 -> x=34.
        set_tpage(gpu, 0);
        gpu.write_gp0(0x65000000u);
        gpu.write_gp0(xy(106, 20));
        gpu.write_gp0(((7u << 6) | 2u) << 16); // CBA X=2*16=32, Y=7
        gpu.write_gp0(0x00010001u);
        t.check(gpu.vram(106, 20) == cyan,
                "CLUT CBA addresses palette X and Y correctly");
    }

    // 8-bit CLUT: two indices are packed into one VRAM halfword.
    {
        GPU gpu;
        const Half red = rgb15(255, 0, 0);
        const Half blue = rgb15(0, 0, 255);
        gpu.set_vram(64, 0, 0x0201u); // page X base 1 = 64 halfwords
        gpu.set_vram(17, 0, red);
        gpu.set_vram(18, 0, blue);
        set_tpage(gpu, static_cast<Half>((1u << 7) | 1u));
        gpu.write_gp0(0x65000000u);
        gpu.write_gp0(xy(110, 20));
        gpu.write_gp0(0x00010000u); // CLUT x=16
        gpu.write_gp0(0x00010002u); // 2x1
        t.check(gpu.vram(110, 20) == red && gpu.vram(111, 20) == blue,
                "8-bit texture extracts low/high byte indices and resolves CLUT");
    }

    // 15-bit direct: one texture pixel per VRAM halfword, no CLUT.
    {
        GPU gpu;
        const Half a = rgb15(255, 64, 0);
        const Half b = rgb15(0, 255, 64);
        gpu.set_vram(256, 0, a);
        gpu.set_vram(257, 0, b);
        set_tpage(gpu, static_cast<Half>((2u << 7) | 4u));
        gpu.write_gp0(0x65000000u);
        gpu.write_gp0(xy(120, 20));
        gpu.write_gp0(0x00000000u);
        gpu.write_gp0(0x00010002u);
        t.check(gpu.vram(120, 20) == a && gpu.vram(121, 20) == b,
                "15-bit texture addresses one texel per VRAM halfword");
    }

    // TPage Y bit selects the upper 256-row texture page.
    {
        GPU gpu;
        const Half magenta = rgb15(255, 0, 255);
        gpu.set_vram(0, 256, 0x0001u);
        gpu.set_vram(17, 0, magenta);
        set_tpage(gpu, static_cast<Half>(1u << 4));
        gpu.write_gp0(0x65000000u);
        gpu.write_gp0(xy(121, 20));
        gpu.write_gp0(0x00010000u);
        gpu.write_gp0(0x00010001u);
        t.check(gpu.vram(121, 20) == magenta,
                "Texture page Y bit selects the 256-row VRAM page");
    }
}

void test_textured_polygon_uv_and_pages(Runner& t)
{
    GPU gpu;
    const Half red = rgb15(255, 0, 0);
    const Half green = rgb15(0, 255, 0);
    const Half blue = rgb15(0, 0, 255);

    // A 4-bit raw texture triangle uses the CLUT from UV0 and TPage from UV1.
    gpu.set_vram(0, 0, 0x0321u);
    gpu.set_vram(0, 0 + 1, 0x0000u);
    gpu.set_vram(0, 0, 0x0321u);
    gpu.set_vram(0, 0, 0x0321u);
    gpu.set_vram(0, 0, 0x0321u);
    gpu.set_vram(0, 0, 0x0321u);
    gpu.set_vram(17, 0, red); gpu.set_vram(18, 0, green); gpu.set_vram(19, 0, blue);

    // Command 25h = flat, raw, textured triangle. UV words carry CLUT/TPage.
    gpu.write_gp0(0x25000000u);
    gpu.write_gp0(xy(10, 10)); gpu.write_gp0(0x00000000u | (1u << 16));
    gpu.write_gp0(xy(20, 10)); gpu.write_gp0(0x00000000u | (0u << 16));
    gpu.write_gp0(xy(10, 20)); gpu.write_gp0(0x00000000u);

    t.check(gpu.vram(11, 11) == red,
            "Textured polygon uses UV coordinates and CLUT/TPage attributes");

    // Texture color 0000h is transparent and must not overwrite the framebuffer.
    gpu.set_vram(0, 0, 0x0000u);
    gpu.set_vram(30, 30, blue);
    gpu.write_gp0(0x25000000u);
    gpu.write_gp0(xy(30, 30)); gpu.write_gp0(0x00000000u | (1u << 16));
    gpu.write_gp0(xy(31, 30)); gpu.write_gp0(0x00000000u);
    gpu.write_gp0(xy(30, 31)); gpu.write_gp0(0x00000000u);
    t.check(gpu.vram(30, 30) == blue,
            "Texture index/color 0000h is treated as transparent");
}

void test_texture_window(Runner& t)
{
    GPU gpu;
    const Half red = rgb15(255, 0, 0);
    const Half green = rgb15(0, 255, 0);
    gpu.set_vram(0, 0, 0x0021u); // U=0 -> index 1, U=1 -> index 2
    gpu.set_vram(17, 0, red);
    gpu.set_vram(18, 0, green);
    set_tpage(gpu, 0);

    // Mask X=1 (8-pixel window), offset X=0: U bit 3 is cleared/repeated.
    gpu.write_gp0(0xE2000001u);
    gpu.write_gp0(0x65000000u);
    gpu.write_gp0(xy(40, 40));
    gpu.write_gp0(0x00010000u);
    gpu.write_gp0(0x00010009u);
    t.check(gpu.vram(40, 40) == red && gpu.vram(41, 40) == green && gpu.vram(48, 40) == red,
            "Texture window modifies UV addressing before sampling");
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
    TestLog::instance() << "\n=== Stage 5: PSX texture rasterization ===\n";
    TestLog::instance() << "VRAM / flat + Gouraud polygons / rectangles / 4-bit CLUT / 8-bit CLUT / 15-bit direct / UV / texture window / clipping\n";
    test_framebuffer(t);
    test_gouraud_shading(t);
    test_triangles(t);
    test_rectangles_and_sprites(t);
    test_textured_rectangles(t);
    test_textured_polygon_uv_and_pages(t);
    test_texture_window(t);
    test_clipping(t);
    TestLog::instance() << "Stage 5 result: " << t.passed << " passed, " << t.failed << " failed.\n";
    return t.failed == 0;
}
} // namespace imatfe::tests
