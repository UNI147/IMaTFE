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
using Half = imatfe::core::u16;

void test_framebuffer(Runner& t)
{
    GPU gpu;
    t.check(gpu.vram().size() == GPU::VRAM_WIDTH * GPU::VRAM_HEIGHT,
            "VRAM contains 1024x512 16-bit pixels");
    t.check(gpu.vram(0, 0) == 0 && gpu.vram(GPU::VRAM_WIDTH - 1, GPU::VRAM_HEIGHT - 1) == 0,
            "VRAM starts cleared");

    constexpr Half pattern[] = {0x0000u, 0x0001u, 0x7FFFu, 0x8000u, 0xFFFFu};
    for (std::size_t i = 0; i < std::size(pattern); ++i)
    {
        gpu.set_vram(i, 0, pattern[i]);
        t.check(gpu.vram(i, 0) == pattern[i], "16-bit pixel round-trip " + std::to_string(i));
    }

    gpu.set_vram(GPU::VRAM_WIDTH - 1, GPU::VRAM_HEIGHT - 1, 0xA55Au);
    t.check(gpu.vram(GPU::VRAM_WIDTH - 1, GPU::VRAM_HEIGHT - 1) == 0xA55Au,
            "Last VRAM pixel is addressable");
    gpu.set_vram(GPU::VRAM_WIDTH, 0, 0x1234u);
    gpu.set_vram(0, GPU::VRAM_HEIGHT, 0x1234u);
    t.check(gpu.vram(GPU::VRAM_WIDTH, 0) == 0 && gpu.vram(0, GPU::VRAM_HEIGHT) == 0,
            "Out-of-range reads return zero");
    t.check(gpu.vram(0, 0) == 0, "Out-of-range writes do not alter VRAM");

    gpu.clear_vram(0xBEEFu);
    t.check(gpu.vram(0, 0) == 0xBEEFu &&
            gpu.vram(GPU::VRAM_WIDTH - 1, GPU::VRAM_HEIGHT - 1) == 0xBEEFu,
            "clear_vram fills the entire framebuffer");
    gpu.clear_vram();
    t.check(gpu.vram(0, 0) == 0 && gpu.vram(17, 23) == 0,
            "clear_vram defaults to black");
}
} // namespace

bool run_stage5_tests()
{
    Runner t;
    TestLog::instance() << "\n=== Stage 5: VRAM framebuffer ===\n";
    test_framebuffer(t);
    TestLog::instance() << "Stage 5 result: " << t.passed << " passed, " << t.failed << " failed.\n";
    return t.failed == 0;
}

} // namespace imatfe::tests
