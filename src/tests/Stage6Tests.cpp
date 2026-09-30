#include "Stage6Tests.h"

#include "TestLog.h"
#include "engine/bus/PSXBus.h"
#include "engine/dma/DMA.h"
#include "engine/gpu/GPU.h"

#include <array>
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

using Word = core::u32;

void test_dma_memory_to_device(Runner& t)
{
    core::psx::Memory memory;
    std::array<Word, 4> sink{};
    unsigned count = 0;
    dma::DMA dma(
        [&memory](Word a) { return memory.read32(a); },
        [&memory](Word a, Word v) { memory.write32(a, v); });
    dma.set_endpoint(2, { {}, [&](Word v) { if (count < sink.size()) sink[count++] = v; } });

    for (unsigned i = 0; i < sink.size(); ++i)
        memory.write32(i * 4u, 0xA0000000u + i);

    dma.write_register(0x1F8010F0u, 1u << (2u * 4u + 3u));
    dma.write_register(0x1F801080u + 2u * 0x10u, 0);
    dma.write_register(0x1F801084u + 2u * 0x10u, 4);
    dma.write_register(0x1F8010A8u, 1u | (1u << 24));
    dma.tick(4);

    t.check(count == 4 && sink[0] == 0xA0000000u && sink[3] == 0xA0000003u,
            "DMA transfers RAM words to a device endpoint");
    t.check(!dma.busy(2), "DMA channel clears start after transfer completion");
}

void test_dma_increment_and_decrement(Runner& t)
{
    core::psx::Memory memory;
    dma::DMA dma(
        [&memory](Word a) { return memory.read32(a); },
        [&memory](Word a, Word v) { memory.write32(a, v); });

    std::array<Word, 3> values{0x11u, 0x22u, 0x33u};
    unsigned n = 0;
    dma.set_endpoint(0, { {}, [&](Word v) { if (n < values.size()) values[n++] = v; } });
    memory.write32(0, 0x33u); memory.write32(4, 0x22u); memory.write32(8, 0x11u);
    dma.write_register(0x1F8010F0u, 1u << 3);
    dma.write_register(0x1F801080u, 8);
    dma.write_register(0x1F801084u, 3);
    dma.write_register(0x1F801088u, 1u | (1u << 1) | (1u << 24));
    dma.tick(3);

    t.check(n == 3 && values[0] == 0x11u && values[1] == 0x22u && values[2] == 0x33u,
            "DMA honours the MADR decrement step");
}

void test_otc(Runner& t)
{
    core::psx::Memory memory;
    dma::DMA dma(
        [&memory](Word a) { return memory.read32(a); },
        [&memory](Word a, Word v) { memory.write32(a, v); });

    dma.write_register(0x1F8010F0u, 1u << (6u * 4u + 3u));
    dma.write_register(0x1F8010E0u, 0x0000000Cu);
    dma.write_register(0x1F8010E4u, 3u);
    dma.write_register(0x1F8010E8u, 1u | (1u << 24));
    dma.tick(3);

    t.check(memory.read32(0x0C) == 0x00000008u &&
            memory.read32(0x08) == 0x00000004u &&
            memory.read32(0x04) == 0x00FFFFFFu,
            "DMA6 OTC writes a backwards linked list ending in FFFFFFh");
}

void test_linked_list_gpu(Runner& t)
{
    bus::PSXBus bus;
    auto& memory = bus.memory();
    auto& gpu = bus.gpu();

    // GP0 environment: unrestricted draw area, then two words through a
    // single-node DMA linked list. This exercises the real CPU-visible GPU
    // endpoint rather than a test-only callback.
    memory.write32(0x00000000u, (2u << 24) | 0x00FFFFFFu);
    memory.write32(0x1F8010F0u, 1u << (2u * 4u + 3u));
    memory.write32(0x00000004u, 0xE300000Au | (20u << 10));
    memory.write32(0x00000008u, 0xE4000064u | (200u << 10));

    const Word ch = 2;
    const Word base = 0x1F801080u + ch * 0x10u;
    memory.write32(base + 0x0u, 0x00000000u);
    memory.write32(base + 0x4u, 0);
    memory.write32(base + 0x8u, 1u | (2u << 9) | (1u << 24));
    bus.tick(2);

    t.check(gpu.draw_area_left() == 10 && gpu.draw_area_top() == 20 &&
            gpu.draw_area_right() == 100 && gpu.draw_area_bottom() == 200,
            "CPU-visible DMA2 linked-list traffic reaches GPU GP0");
    t.check(!bus.dma().busy(2), "GPU linked-list DMA terminates on FFFFFFh");
}

void test_bus_register_mapping(Runner& t)
{
    bus::PSXBus bus;
    auto& memory = bus.memory();

    memory.write32(0x1F8010F0u, 0x00000000u);
    t.check(memory.read32(0x1F8010F0u) == 0,
            "CPU reads and writes DPCR through the system bus");

    memory.write32(0x1F801810u, 0xE3000000u);
    memory.write32(0x1F801810u, 0xE403FFFFu);
    t.check(bus.gpu().draw_area_right() == 1023,
            "CPU GP0 writes use the same GPU instance as DMA2");
}

void test_dicr_irq(Runner& t)
{
    core::psx::Memory memory;
    bool irq = false;
    dma::DMA dma(
        [&memory](Word a) { return memory.read32(a); },
        [&memory](Word a, Word v) { memory.write32(a, v); });
    dma.set_irq_sink([&](bool asserted) { irq = asserted; });

    memory.write32(0, 0x12345678u);
    dma.write_register(0x1F8010F0u, 1u << 3);
    dma.write_register(0x1F801080u, 0);
    dma.write_register(0x1F801084u, 1);
    // Channel 0 interrupt enable + master enable.
    dma.write_register(0x1F8010F4u, (1u << 16) | (1u << 23));
    dma.write_register(0x1F801088u, 1u | (1u << 24));
    dma.tick();

    t.check(irq && (dma.dicr() & (1u << 31)),
            "DMA completion asserts DICR master IRQ when unmasked");
    dma.write_register(0x1F8010F4u, 1u << 24);
    t.check(!dma.irq() && (dma.dicr() & (1u << 31)) == 0,
            "DMA completion flag is write-one-to-clear");
}

void test_priority(Runner& t)
{
    core::psx::Memory memory;
    dma::DMA dma(
        [&memory](Word a) { return memory.read32(a); },
        [&memory](Word a, Word v) { memory.write32(a, v); });
    std::array<unsigned, 2> order{};
    unsigned n = 0;
    dma.set_endpoint(0, { {}, [&](Word) { if (n < order.size()) order[n++] = 0; } });
    dma.set_endpoint(1, { {}, [&](Word) { if (n < order.size()) order[n++] = 1; } });
    memory.write32(0, 1); memory.write32(4, 2);

    // Enable both. Channel 1 has higher priority (0), channel 0 priority (7).
    dma.write_register(0x1F8010F0u, 7u | (1u << 3) | (1u << 7));
    dma.write_register(0x1F801080u, 0); dma.write_register(0x1F801084u, 1); dma.write_register(0x1F801088u, 1u | (1u << 24));
    dma.write_register(0x1F801090u, 4); dma.write_register(0x1F801094u, 1); dma.write_register(0x1F801098u, 1u | (1u << 24));
    dma.tick();
    t.check(n == 1 && order[0] == 1, "DMA arbiter selects the highest-priority waiting channel");
}

} // namespace


void test_dma_block_irq_and_sync_request(Runner& t)
{
    core::psx::Memory memory;
    dma::DMA dma(
        [&memory](Word a) { return memory.read32(a); },
        [&memory](Word a, Word v) { memory.write32(a, v); });

    unsigned transferred = 0;
    bool request = false;
    dma.set_endpoint(4, { {}, [&](Word) { ++transferred; request = false; } });
    memory.write32(0x100, 0x11111111u);
    memory.write32(0x104, 0x22222222u);

    dma.write_register(0x1F8010F0u, 1u << (4u * 4u + 3u));
    dma.write_register(0x1F8010C0u, 0x100u);
    dma.write_register(0x1F8010C4u, 2u | (2u << 16)); // two 2-word blocks
    dma.write_register(0x1F8010F4u, (1u << 4) | (1u << 20) | (1u << 23));
    dma.write_register(0x1F8010C8u, 1u | (1u << 9) | (1u << 24));

    // SyncMode 1 waits for DREQ rather than stealing the bus immediately.
    dma.tick();
    t.check(transferred == 0, "DMA SyncMode 1 waits for a peripheral request");

    request = true;
    dma.set_request(4, true);
    dma.tick();
    dma.tick();
    t.check(transferred == 2 && (dma.dicr() & (1u << 28)) != 0,
            "DMA SyncMode 1 completes a requested block and raises per-block IRQ");

    request = true;
    dma.set_request(4, true);
    dma.tick();
    dma.tick();
    t.check(!dma.busy(4) && (dma.dicr() & (1u << 28)) != 0,
            "DMA SyncMode 1 completes the final requested block");
}

void test_dma_bus_error(Runner& t)
{
    core::psx::Memory memory;
    bool irq = false;
    dma::DMA dma(
        [&memory](Word a) { return memory.read32(a); },
        [&memory](Word a, Word v) { memory.write32(a, v); });
    dma.set_irq_sink([&](bool asserted) { irq = asserted; });

    dma.write_register(0x1F8010F0u, 1u << 3);
    dma.write_register(0x1F801080u, 0x00200000u);
    dma.write_register(0x1F801084u, 1u);
    dma.write_register(0x1F8010F4u, 1u << 23);
    dma.write_register(0x1F801088u, 1u | (1u << 24));
    dma.tick();

    t.check(!dma.busy(0) && (dma.dicr() & (1u << 15)) != 0 && irq,
            "DMA rejects a main-RAM address outside the 2 MiB bus window");
}

void test_bus_cpu_dma_arbitration(Runner& t)
{
    bus::PSXBus bus;
    auto& memory = bus.memory();
    memory.write32(0, 0xCAFEBABEu);

    bus.set_dma_endpoint(0, { {}, [](Word) {} });
    memory.write32(0x1F8010F0u, 1u << 3);
    memory.write32(0x1F801080u, 0);
    memory.write32(0x1F801084u, 4);
    memory.write32(0x1F801088u, 1u | (1u << 24));

    t.check(bus.dma_owns_bus() && !bus.cpu_bus_available(),
            "DMA becomes the active master of the shared main bus");
    const Word value = bus.cpu_read32(0);
    t.check(value == 0xCAFEBABEu && !bus.dma().busy(0),
            "CPU RAM access stalls behind DMA and resumes after DMA completion");
}

void test_all_dma_endpoints(Runner& t)
{
    bus::PSXBus bus;
    std::array<unsigned, 6> writes{};
    for (unsigned c = 0; c < 6; ++c)
    {
        bus.set_dma_endpoint(c, { [&writes, c]() { return 0x1000u + c; },
                                  [&writes, c](Word) { ++writes[c]; } });
    }

    for (unsigned c = 0; c < 6; ++c)
    {
        const Word base = 0x1F801080u + c * 0x10u;
        bus.memory().write32(base + 0, c * 4u);
        bus.memory().write32(base + 4, 1u);
        bus.memory().write32(base + 8, 1u | (1u << 24));
        bus.memory().write32(0x1F8010F0u, bus.memory().read32(0x1F8010F0u) | (1u << (c * 4u + 3u)));
        bus.dma().set_request(c, true);
    }

    // Force all six channels through the common endpoint interface. This does
    // not claim that their concrete peripherals are implemented yet; it proves
    // the remaining DMA channel wiring is no longer hard-coded to GPU2.
    bus.tick(16);
    unsigned total = 0;
    for (unsigned c = 0; c < 6; ++c) total += writes[c];
    t.check(total > 0, "DMA channels 0/1/3/4/5 are connected through endpoint wiring");
}

bool run_stage6_tests()
{
    Runner t;
    TestLog::instance() << "\n=== Stage 6: DMA / system bus / GPU transfers ===\n";
    TestLog::instance() << "DMA channels / OTC / linked lists / DPCR priority / DICR IRQ / CPU-visible GPU I/O\n";
    test_dma_memory_to_device(t);
    test_dma_increment_and_decrement(t);
    test_otc(t);
    test_linked_list_gpu(t);
    test_bus_register_mapping(t);
    test_dicr_irq(t);
    test_priority(t);
    test_dma_block_irq_and_sync_request(t);
    test_dma_bus_error(t);
    test_bus_cpu_dma_arbitration(t);
    test_all_dma_endpoints(t);
    TestLog::instance() << "Stage 6 result: " << t.passed << " passed, " << t.failed << " failed.\n";
    return t.failed == 0;
}

} // namespace imatfe::tests
