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
    gpu.write_gp0(0xE407FFFFu);
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


void set_tpage(GPU& gpu, Half tpage);

void test_lines(Runner& t)
{
    const Half red = rgb15(255, 0, 0);
    const Half green = rgb15(0, 255, 0);
    const Word red_rgb = 0x000000FFu;
    const Word green_rgb = 0x0000FF00u;
    GPU gpu;
    unrestricted(gpu);

    gpu.write_gp0(0x400000FFu); gpu.write_gp0(xy(10, 10)); gpu.write_gp0(xy(15, 10));
    t.check(gpu.vram(10, 10) == red && gpu.vram(15, 10) == red, "Flat line includes both endpoints");

    gpu.clear_vram();
    gpu.write_gp0(0x500000FFu); gpu.write_gp0(xy(10, 10)); gpu.write_gp0(green_rgb); gpu.write_gp0(xy(15, 10));
    t.check(gpu.vram(10, 10) == red && gpu.vram(15, 10) == green &&
            gpu.vram(12, 10) != red && gpu.vram(12, 10) != green, "Gouraud line interpolates endpoint colors");

    gpu.clear_vram();
    gpu.write_gp0(0x480000FFu); gpu.write_gp0(xy(10, 10)); gpu.write_gp0(xy(15, 10));
    gpu.write_gp0(xy(15, 15)); gpu.write_gp0(0x50005000u);
    t.check(gpu.vram(15, 10) == red && gpu.vram(15, 15) == red, "Flat polyline renders all segments and terminates");

    gpu.clear_vram();
    gpu.write_gp0(0x580000FFu); gpu.write_gp0(xy(10, 10)); gpu.write_gp0(green_rgb); gpu.write_gp0(xy(15, 10));
    gpu.write_gp0(red_rgb); gpu.write_gp0(xy(15, 15)); gpu.write_gp0(0x50005000u);
    t.check(gpu.vram(10, 10) == red && gpu.vram(15, 10) == green && gpu.vram(15, 15) == red,
            "Gouraud polyline interpolates each segment");

    gpu.clear_vram();
    gpu.write_gp0(0x440000FFu); gpu.write_gp0(xy(20, 20)); gpu.write_gp0(xy(24, 20));
    t.check(gpu.vram(22, 20) == red, "Line bit 26 is reserved; PSX has no textured-line mode");

    gpu.clear_vram();
    gpu.write_gp0(0x400000FFu); gpu.write_gp0(xy(30, 30)); gpu.write_gp0(xy(30, 30));
    t.check(gpu.vram(30, 30) == red, "Zero-length line draws one pixel");

    gpu.clear_vram();
    gpu.write_gp0(0x420000FFu); gpu.write_gp0(xy(30, 30)); gpu.write_gp0(xy(32, 30));
    t.check(gpu.vram(31, 30) != 0 && gpu.vram(31, 30) != red, "Semi-transparent line uses the line blend path");

    {
        GPU span; unrestricted(span);
        span.write_gp0(0x400000FFu); span.write_gp0(xy(0, 40)); span.write_gp0(xy(1023, 40));
        t.check(span.vram(0, 40) == red && span.vram(1023, 40) == red, "Line with 1023-pixel X span is accepted");
    }
    {
        GPU span; unrestricted(span);
        span.write_gp0(0x400000FFu); span.write_gp0(xy(-1024, 40)); span.write_gp0(xy(0, 40));
        t.check(span.vram(0, 40) == 0, "Line with X span above 1023 pixels is rejected");
    }
    {
        GPU span; unrestricted(span);
        span.write_gp0(0x400000FFu); span.write_gp0(xy(50, 0)); span.write_gp0(xy(50, 511));
        t.check(span.vram(50, 0) == red && span.vram(50, 511) == red, "Line with 511-pixel Y span is accepted");
    }
    {
        GPU span; unrestricted(span);
        span.write_gp0(0x400000FFu); span.write_gp0(xy(50, -1024)); span.write_gp0(xy(50, 0));
        t.check(span.vram(50, 0) == 0, "Line with Y span above 511 pixels is rejected");
    }
}

void test_polygon_variants(Runner& t)
{
    const Half red = rgb15(255, 0, 0);
    const Half green = rgb15(0, 255, 0);
    const Half blue = rgb15(0, 0, 255);
    const Word green_rgb = 0x0000FF00u;
    const Word blue_rgb = 0x00FF0000u;
    GPU gpu;
    unrestricted(gpu);

    gpu.write_gp0(0x200000FFu); gpu.write_gp0(xy(40, 40)); gpu.write_gp0(xy(50, 40)); gpu.write_gp0(xy(40, 50));
    t.check(gpu.vram(41, 41) == red, "Flat triangle variant");

    gpu.clear_vram();
    gpu.write_gp0(0x300000FFu); gpu.write_gp0(xy(40, 40)); gpu.write_gp0(green_rgb); gpu.write_gp0(xy(50, 40));
    gpu.write_gp0(blue_rgb); gpu.write_gp0(xy(40, 50));
    t.check(gpu.vram(41, 41) != red && gpu.vram(41, 41) != green && gpu.vram(41, 41) != blue, "Gouraud triangle variant");

    gpu.clear_vram();
    gpu.write_gp0(0x280000FFu); gpu.write_gp0(xy(40, 40)); gpu.write_gp0(xy(50, 40)); gpu.write_gp0(xy(50, 50)); gpu.write_gp0(xy(40, 50));
    t.check(gpu.vram(45, 43) == red, "Flat quad variant");

    gpu.clear_vram();
    gpu.write_gp0(0x380000FFu); gpu.write_gp0(xy(40, 40)); gpu.write_gp0(green_rgb); gpu.write_gp0(xy(50, 40));
    gpu.write_gp0(blue_rgb); gpu.write_gp0(xy(50, 50)); gpu.write_gp0(0x00FFFF00u); gpu.write_gp0(xy(40, 50));
    t.check(gpu.vram(45, 45) != 0, "Gouraud quad variant");

    gpu.clear_vram();
    gpu.set_vram(0, 0, 0x0001u); gpu.set_vram(17, 0, red);
    gpu.write_gp0(0x25000000u); gpu.write_gp0(xy(60, 60)); gpu.write_gp0(0x00010000u);
    gpu.write_gp0(xy(64, 60)); gpu.write_gp0(0); gpu.write_gp0(xy(60, 64)); gpu.write_gp0(0);
    t.check(gpu.vram(61, 61) == red, "Raw 4-bit textured polygon");

    gpu.clear_vram(); gpu.set_vram(0, 0, 0x0001u); gpu.set_vram(17, 0, rgb15(128, 255, 128));
    gpu.write_gp0(0x240000FFu); gpu.write_gp0(xy(60, 60)); gpu.write_gp0(0x00010000u);
    gpu.write_gp0(xy(64, 60)); gpu.write_gp0(0); gpu.write_gp0(xy(60, 64)); gpu.write_gp0(0);
    t.check(gpu.vram(61, 60) != 0, "Modulated 4-bit textured polygon");

    gpu.clear_vram(); gpu.set_vram(64, 0, 0x0001u); gpu.set_vram(17, 0, blue);
    gpu.write_gp0(0x25000000u); gpu.write_gp0(xy(70, 60)); gpu.write_gp0(0x00010000u);
    gpu.write_gp0(xy(74, 60)); gpu.write_gp0(0x00810000u); gpu.write_gp0(xy(70, 64)); gpu.write_gp0(0x00810000u);
    t.check(gpu.vram(71, 60) == blue, "8-bit CLUT textured polygon");

    gpu.clear_vram(); const Half direct = rgb15(64, 192, 32); gpu.set_vram(128, 0, direct);
    gpu.write_gp0(0x25000000u); gpu.write_gp0(xy(80, 60)); gpu.write_gp0(0);
    gpu.write_gp0(xy(84, 60)); gpu.write_gp0(0x01020000u); gpu.write_gp0(xy(80, 64)); gpu.write_gp0(0);
    t.check(gpu.vram(81, 61) == direct, "15-bit direct textured polygon");
}

void test_rectangle_variants(Runner& t)
{
    const Half red = rgb15(255, 0, 0);
    GPU gpu; unrestricted(gpu);
    gpu.write_gp0(0x680000FFu); gpu.write_gp0(xy(90, 90));
    t.check(gpu.vram(90, 90) == red && gpu.vram(91, 90) == 0, "1x1 fixed rectangle variant");

    gpu.write_gp0(0x600000FFu); gpu.write_gp0(xy(92, 90)); gpu.write_gp0(0);
    t.check(gpu.vram(92, 90) == 0, "Variable rectangle zero size draws nothing");

    gpu.clear_vram(); gpu.set_vram(0, 0, 1); gpu.set_vram(17, 0, red); set_tpage(gpu, 0);
    gpu.write_gp0(0x75000000u); gpu.write_gp0(xy(100, 100)); gpu.write_gp0(0x00010000u);
    t.check(gpu.vram(100, 100) == red, "8x8 raw textured rectangle variant");

    gpu.clear_vram(); gpu.set_vram(0, 0, 0x0010u); gpu.set_vram(16, 0, 0); gpu.set_vram(17, 0, red);
    set_tpage(gpu, static_cast<Half>(1u << 12));
    gpu.write_gp0(0x65000000u); gpu.write_gp0(xy(110, 100)); gpu.write_gp0(0x00010001u); gpu.write_gp0(0x00010002u);
    t.check(gpu.vram(110, 100) == red, "Textured rectangle X-flip uses TPage bit 12");
}

void test_vram_transfers(Runner& t)
{
    const Half a = 0x1234u, b = 0x5678u, c = 0x9ABCu, d = 0xDEF0u;
    {
        GPU gpu;
        gpu.write_gp0(0xA0000000u); gpu.write_gp0(xy(1023, 511)); gpu.write_gp0(0x00020003u);
        gpu.write_gp0(static_cast<Word>(a) | (static_cast<Word>(b) << 16));
        gpu.write_gp0(static_cast<Word>(c) | (static_cast<Word>(d) << 16));
        t.check(gpu.vram(1023, 511) == a && gpu.vram(0, 511) == b && gpu.vram(1, 511) == c,
                "CPU-to-VRAM handles odd width and horizontal wrapping");

        GPU masked;
        masked.set_vram(10, 10, 0x8000u);
        masked.write_gp0(0xE6000002u);
        masked.write_gp0(0xA0000000u); masked.write_gp0(xy(10, 10)); masked.write_gp0(0x00010001u);
        masked.write_gp0(a);
        t.check(masked.vram(10, 10) == 0x8000u, "CPU-to-VRAM mask check protects a masked destination");

        masked.write_gp0(0xE6000001u);
        masked.write_gp0(0xA0000000u); masked.write_gp0(xy(11, 10)); masked.write_gp0(0x00010001u);
        masked.write_gp0(a);
        t.check((masked.vram(11, 10) & 0x8000u) != 0, "CPU-to-VRAM mask set marks written pixels");
    }
    {
        GPU gpu; gpu.set_vram(10, 20, a); gpu.set_vram(11, 20, b); gpu.set_vram(12, 20, c);
        gpu.write_gp0(0xC0000000u); gpu.write_gp0(xy(10, 20)); gpu.write_gp0(0x00010003u);
        t.check((gpu.read_gp1() & (1u << 27)) != 0, "VRAM-to-CPU asserts read-FIFO-ready");
        const Word first = gpu.read_gp0(), second = gpu.read_gp0();
        t.check(first == (static_cast<Word>(a) | (static_cast<Word>(b) << 16)) &&
                (second & 0xFFFFu) == c && (second >> 16) == 0, "VRAM-to-CPU packs odd transfers with zero padding");
        t.check((gpu.read_gp1() & (1u << 27)) == 0, "VRAM-to-CPU clears ready after final word");
    }
    {
        GPU gpu; gpu.set_vram(20, 20, a); gpu.set_vram(21, 20, b); gpu.set_vram(22, 20, c);
        gpu.write_gp0(0x80000000u); gpu.write_gp0(xy(20, 20)); gpu.write_gp0(xy(21, 20)); gpu.write_gp0(0x00010003u);
        t.check(gpu.vram(21, 20) == a && gpu.vram(22, 20) == b && gpu.vram(23, 20) == c, "VRAM-to-VRAM overlap preserves source ordering");

        gpu.clear_vram(); gpu.set_vram(1023, 511, a); gpu.set_vram(0, 511, b); gpu.set_vram(0, 0, c);
        gpu.write_gp0(0x80000000u); gpu.write_gp0(xy(1023, 511)); gpu.write_gp0(xy(10, 10)); gpu.write_gp0(0x00020003u);
        t.check(gpu.vram(10, 10) == a && gpu.vram(11, 10) == b && gpu.vram(12, 10) == 0 &&
                gpu.vram(10, 11) == 0 && gpu.vram(11, 11) == c && gpu.vram(12, 11) == 0, "VRAM-to-VRAM wraps source across both axes");

        gpu.clear_vram(); gpu.set_vram(30, 30, 0x8001u); gpu.set_vram(31, 30, b); gpu.set_vram(40, 30, 0x8002u);
        gpu.write_gp0(0xE6000002u); gpu.write_gp0(0x80000000u); gpu.write_gp0(xy(30, 30)); gpu.write_gp0(xy(40, 30)); gpu.write_gp0(0x00010002u);
        t.check(gpu.vram(40, 30) == 0x8002u && gpu.vram(41, 30) == b, "VRAM-to-VRAM mask check protects masked destination");

        gpu.write_gp0(0xE6000001u); gpu.write_gp0(0x80000000u); gpu.write_gp0(xy(31, 30)); gpu.write_gp0(xy(50, 30)); gpu.write_gp0(0x00010001u);
        t.check((gpu.vram(50, 30) & 0x8000u) != 0, "VRAM-to-VRAM mask set marks copied pixels");
    }
    {
        GPU gpu;
        gpu.write_gp0(0x020000FFu); gpu.write_gp0(xy(17, 40)); gpu.write_gp0(0x00010001u);
        const Half red = rgb15(255, 0, 0);
        t.check(gpu.vram(16, 40) == red && gpu.vram(31, 40) == red && gpu.vram(32, 40) == 0, "Quick fill aligns X to 16-pixel blocks");
        gpu.set_vram(16, 41, 0x8000u); gpu.write_gp0(0xE6000003u);
        gpu.write_gp0(0x020000FFu); gpu.write_gp0(xy(16, 41)); gpu.write_gp0(0x00010001u);
        t.check(gpu.vram(16, 41) == red, "Quick fill ignores mask state");
        gpu.set_vram(0, 50, 0x7777u); gpu.write_gp0(0x020000FFu); gpu.write_gp0(xy(0, 50)); gpu.write_gp0(0x00000400u);
        t.check(gpu.vram(0, 50) == 0x7777u, "Quick fill raw width 0x400 becomes zero");
    }
}

void test_gpu_environment(Runner& t)
{
    GPU gpu;
    gpu.write_gp0(0xE1000265u);
    t.check((gpu.read_gp1() & 0x1FFFu) == 0x0265u, "GP0(E1h) updates TPage/ABR/dither status fields");

    gpu.write_gp0(0xE2F12345u); gpu.write_gp0(0xE3001805u); gpu.write_gp0(0xE4002007u); gpu.write_gp0(0xE53FFFFEu);
    gpu.write_gp1(0x10000002u); t.check(gpu.read_gp0() == 0x00012345u, "GP1(10h) reads texture-window environment");
    gpu.write_gp1(0x11000003u); t.check(gpu.read_gp0() == 0x00001805u, "GP1(11h) mirror reads draw-area top-left");
    gpu.write_gp1(0x12000004u); t.check(gpu.read_gp0() == 0x00002007u, "GP1(12h) mirror reads draw-area bottom-right");
    gpu.write_gp1(0x13000005u); t.check(gpu.read_gp0() == 0x003FFFFEu, "GP1(13h) mirror reads signed draw offset");

    gpu.write_gp0(0xE6000003u); t.check((gpu.read_gp1() & (3u << 11)) == (3u << 11), "GP0(E6h) exposes mask-set/check in GPUSTAT");
    gpu.write_gp1(0x03000001u); t.check((gpu.read_gp1() & (1u << 23)) == 0, "GP1(03h) display disable reaches GPUSTAT");
    gpu.write_gp1(0x03000000u);
    gpu.write_gp1(0x04000000u);
    t.check(((gpu.read_gp1() >> 29) & 3u) == 0u && !gpu.dma_request(), "GP1(04h)=0 disables DREQ");
    gpu.write_gp1(0x04000001u);
    t.check(((gpu.read_gp1() >> 29) & 3u) == 1u && gpu.dma_request(), "GP1(04h)=1 reports write-FIFO-not-full");
    gpu.write_gp1(0x04000002u);
    t.check(((gpu.read_gp1() >> 29) & 3u) == 2u && gpu.dma_request(), "GP1(04h)=2 reports write-FIFO-empty");
    gpu.write_gp1(0x04000003u);
    t.check(((gpu.read_gp1() >> 29) & 3u) == 3u && !gpu.dma_request(), "GP1(04h)=3 waits for a VRAM read transfer");
    gpu.write_gp1(0x05001234u); gpu.write_gp1(0x06054321u); gpu.write_gp1(0x07098765u); gpu.write_gp1(0x0800003Fu);
    const Word stat = gpu.read_gp1();
    t.check(((stat >> 17) & 3u) == 3u && ((stat >> 19) & 7u) == 7u, "GP1(05h..08h) display state reaches GPUSTAT");
    gpu.set_vram(0, 0, 0x1234u); gpu.write_gp0(0xC0000000u); gpu.write_gp0(xy(0, 0)); gpu.write_gp0(0x00010001u);
    t.check(((gpu.read_gp1() >> 29) & 3u) == 3u && gpu.dma_request(), "GP1(04h)=3 asserts DREQ while VRAM read data is ready");
    (void)gpu.read_gp0();
    gpu.write_gp0(0x1F000000u); t.check((gpu.read_gp1() & (1u << 24)) != 0, "GP0(1Fh) raises GPU IRQ");
    gpu.write_gp1(0x02000000u); t.check((gpu.read_gp1() & (1u << 24)) == 0, "GP1(02h) acknowledges GPU IRQ");
    gpu.write_gp1(0x00000000u); t.check(gpu.read_gp1() == 0x14802000u, "GP1(00h) restores GPU status reset state");
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
    gpu.write_gp0(0xE407FFFFu);
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
void test_dithering(Runner& t)
{
    GPU gpu;
    unrestricted(gpu);
    const Half plain = rgb15(8, 8, 8);

    gpu.write_gp0(0xE1000000u); // dithering disabled
    gpu.write_gp0(0x60080808u); gpu.write_gp0(xy(100, 100)); gpu.write_gp0(0x00010004u);
    t.check(gpu.vram(100, 100) == plain && gpu.vram(101, 100) == plain,
            "Dithering disabled preserves ordinary 5-bit quantization");

    gpu.clear_vram();
    gpu.write_gp0(0xE1000200u); // GP0(E1h), dither enable
    // PSX source specifies that rectangles are NOT dithered.
    gpu.write_gp0(0x60080808u); gpu.write_gp0(xy(100, 100)); gpu.write_gp0(0x00010004u);
    t.check(gpu.vram(100, 100) == plain && gpu.vram(101, 100) == plain,
            "Rectangles remain undithered even when DTD is enabled");

    gpu.clear_vram();
    gpu.write_gp0(0x40080808u); gpu.write_gp0(xy(100, 100)); gpu.write_gp0(xy(103, 100));
    t.check(gpu.vram(100, 100) == rgb15(0, 0, 0) &&
            gpu.vram(101, 100) == rgb15(8, 8, 8) &&
            gpu.vram(102, 100) == rgb15(0, 0, 0) &&
            gpu.vram(103, 100) == rgb15(9, 9, 9),
            "Lines are dithered at each framebuffer coordinate");
    gpu.clear_vram();
    gpu.write_gp0(0x40080808u); gpu.write_gp0(xy(104, 100)); gpu.write_gp0(xy(104, 100));
    t.check(gpu.vram(104, 100) == gpu.vram(100, 100),
            "Line dither matrix repeats every four framebuffer pixels");
}

void test_blending_and_stp(Runner& t)
{
    const Half background = rgb15(80, 80, 80);
    const Half foreground = rgb15(40, 40, 40);
    const auto draw = [&](Half mode, Half texel) {
        GPU gpu;
        gpu.set_vram(256, 0, texel); // 15-bit texture page, U=0
        gpu.set_vram(300, 20, background);
        set_tpage(gpu, static_cast<Half>((2u << 7) | (mode << 5) | 4u));
        gpu.write_gp0(0x67000000u); // semi-transparent raw variable textured rectangle
        gpu.write_gp0(xy(300, 20));
        gpu.write_gp0(0x00000000u);
        gpu.write_gp0(0x00010001u);
        return gpu.vram(300, 20);
    };
    t.check(draw(0, static_cast<Half>(foreground | 0x8000u)) == rgb15(60, 60, 60),
            "Blend mode 0 averages source and destination per 5-bit channel");
    t.check(draw(1, static_cast<Half>(foreground | 0x8000u)) == rgb15(120, 120, 120),
            "Blend mode 1 saturates source plus destination");
    t.check(draw(2, static_cast<Half>(foreground | 0x8000u)) == rgb15(40, 40, 40),
            "Blend mode 2 subtracts source from destination");
    t.check(draw(3, static_cast<Half>(foreground | 0x8000u)) == rgb15(90, 90, 90),
            "Blend mode 3 adds one quarter of source");
    t.check(draw(0, foreground) == foreground,
            "STP clear bypasses semi-transparency and writes source directly");
    t.check(draw(0, static_cast<Half>(foreground | 0x8000u)) != background,
            "STP set enables semi-transparent blending");
}


void test_gpu_command_boundaries(Runner& t)
{
    {
        GPU gpu;
        gpu.write_gp0(0xE3FFFFFFu); // Set all payload bits; only X[9:0] and Y[18:10] are retained.
        gpu.write_gp1(0x10000003u);
        t.check(gpu.read_gp0() == 0x0007FFFFu,
                "GP0(E3h) masks drawing-area coordinates to hardware field widths");
        gpu.write_gp0(0xE4FFFFFFu);
        gpu.write_gp1(0x10000004u);
        t.check(gpu.read_gp0() == 0x0007FFFFu,
                "GP0(E4h) masks drawing-area coordinates to hardware field widths");
    }
    {
        GPU gpu;
        gpu.set_vram(1023, 511, 0x1357u);
        gpu.set_vram(0, 511, 0x2468u);
        gpu.write_gp0(0xC0000000u);
        gpu.write_gp0(xy(1023, 511));
        gpu.write_gp0(0x00000000u); // zero width and height encode maximum dimensions.
        const Word first = gpu.read_gp0();
        t.check(first == (0x1357u | (0x2468u << 16)),
                "VRAM-to-CPU zero dimensions normalize to maximum transfer dimensions");
    }
    {
        GPU gpu;
        gpu.write_gp0(0xA0000000u);
        gpu.write_gp0(xy(4, 4));
        gpu.write_gp0(0x00010003u);
        gpu.write_gp0(0xAAAA5555u);
        gpu.write_gp1(0x01000000u); // Reset command buffer while transfer is incomplete.
        gpu.write_gp0(0x200000FFu);
        gpu.write_gp0(xy(10, 10));
        gpu.write_gp0(xy(14, 10));
        gpu.write_gp0(xy(10, 14));
        t.check(gpu.vram(10, 11) == rgb15(255, 0, 0),
                "GP1(01h) aborts an incomplete GP0 packet before the next command");
    }
    {
        GPU gpu;
        gpu.set_vram(1023, 511, 0x1234u);
        gpu.write_gp0(0x80000000u);
        gpu.write_gp0(xy(1023, 511));
        gpu.write_gp0(xy(0, 0));
        gpu.write_gp0(0x00010002u);
        t.check(gpu.vram(0, 0) == 0x1234u,
                "VRAM-to-VRAM transfer wraps at the right and bottom VRAM edges");
    }
}


}

bool run_stage5_tests()
{
    Runner t;
    TestLog::instance() << "\n=== Stage 5: PSX texture rasterization ===\n";
    TestLog::instance() << "VRAM / all PSX line modes / all polygon variants / all rectangle variants / VRAM transfers / GPU environment / textures / clipping / dithering / blending / STP\n";
    test_framebuffer(t);
    test_lines(t);
    test_polygon_variants(t);
    test_rectangle_variants(t);
    test_vram_transfers(t);
    test_gpu_environment(t);
    test_gouraud_shading(t);
    test_triangles(t);
    test_rectangles_and_sprites(t);
    test_textured_rectangles(t);
    test_textured_polygon_uv_and_pages(t);
    test_texture_window(t);
    test_clipping(t);
    test_dithering(t);
    test_blending_and_stp(t);
    test_gpu_command_boundaries(t);
    TestLog::instance() << "Stage 5 result: " << t.passed << " passed, " << t.failed << " failed.\n";
    return t.failed == 0;
}
} // namespace imatfe::tests
