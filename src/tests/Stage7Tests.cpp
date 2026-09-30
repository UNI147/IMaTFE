#include "Stage7Tests.h"
#include "engine/bus/PSXBus.h"
#include "engine/cpu/CPU.h"
#include "engine/core/EventScheduler.h"
#include "engine/core/ClockDomain.h"
#include "TestLog.h"

#include <array>
#include <vector>

namespace imatfe::tests
{
namespace
{
struct Runner
{
    unsigned passed = 0, failed = 0;
    void check(bool value, const char* message)
    {
        TestLog::instance() << (value ? "[PASS] " : "[FAIL] ") << message << '\n';
        value ? ++passed : ++failed;
    }
};


void test_rational_clock_domain(Runner& t)
{
    core::ClockDomain video(11, 7);
    core::u64 produced = 0;
    for (unsigned i = 0; i < 7; ++i) produced += video.advance();
    t.check(produced == 11 && video.ticks() == 11 && video.phase() == 0,
            "rational clock domain preserves exact 11:7 ratio without floating point");
    video.reset();
    t.check(video.advance(3) == 4 && video.phase() == 5,
            "rational clock domain retains fractional phase between advances");
}

void test_video_timing(Runner& t)
{
    gpu::VideoTiming timing;
    timing.tick(2560);
    t.check(timing.hblank() && timing.line() == 0,
            "video timing enters horizontal blank at configured active-area boundary");
    timing.tick(3412 - 2560);
    t.check(timing.line() == 1 && timing.clock_in_line() == 0 && !timing.hblank(),
            "NTSC video timing advances deterministically at scanline boundary");
    timing.set_standard(gpu::VideoTiming::Standard::PAL);
    t.check(timing.profile().clocks_per_line == 3405 && timing.line() == 0,
            "PAL mode selects its scanline profile and resets phase");
    timing.tick(static_cast<core::u64>(3405) * 288);
    t.check(timing.vblank(), "PAL video timing enters vertical blank after active lines");
}

void test_event_order_and_deadlines(Runner& t)
{
    core::EventScheduler scheduler;
    std::vector<int> order;
    scheduler.schedule_at(4, [&] { order.push_back(1); });
    scheduler.schedule_at(2, [&] { order.push_back(0); });
    scheduler.schedule_at(4, [&] { order.push_back(2); });
    scheduler.run_until(3);
    t.check(order.size() == 1 && order[0] == 0 && scheduler.now() == 3,
            "scheduler runs only events due on the shared integer timeline");
    scheduler.run_until(4);
    t.check(order == std::vector<int>({0, 1, 2}),
            "same-tick events execute deterministically in insertion order");
}

void test_cancel_and_reset(Runner& t)
{
    core::EventScheduler scheduler;
    unsigned calls = 0;
    const auto cancelled = scheduler.schedule_after(2, [&] { ++calls; });
    scheduler.schedule_after(3, [&] { ++calls; });
    t.check(scheduler.cancel(cancelled), "scheduled event can be cancelled");
    scheduler.run_until(3);
    t.check(calls == 1, "cancelled event does not execute");
    scheduler.schedule_after(1, [&] { ++calls; });
    scheduler.reset();
    scheduler.run_until(10);
    t.check(scheduler.now() == 10 && calls == 1,
            "reset clears pending events and restarts scheduler time");
}

void test_bus_clock_and_events(Runner& t)
{
    bus::PSXBus bus;
    unsigned observed = 0;
    bus.scheduler().schedule_at(3, [&] { observed = static_cast<unsigned>(bus.cycles()); });
    bus.tick(2);
    t.check(bus.cycles() == 2 && observed == 0, "bus clock advances without firing future events");
    bus.tick();
    t.check(bus.cycles() == 3 && observed == 3 && bus.scheduler().now() == 3,
            "PSXBus and event scheduler advance on the same timeline");
    bus.reset();
    t.check(bus.cycles() == 0 && bus.scheduler().now() == 0,
            "bus reset resets the shared timeline");
}


void test_gte_uses_bus_timeline(Runner& t)
{
    bus::PSXBus bus;
    cpu::CPU cpu(bus);
    cpu.gte().execute(0x06u); // NCLIP: eight GTE clocks in this model.
    const auto initial = cpu.gte().busy_cycles();
    bus.tick(3);
    t.check(initial == 8 && cpu.gte().busy_cycles() == 5,
            "GTE busy countdown advances with the shared bus timeline");
    bus.tick(5);
    t.check(!cpu.gte().busy() && bus.cycles() == 8,
            "GTE completes without a second CPU-local clock source");
}

void test_cpu_instruction_fetch_timing(Runner& t)
{
    bus::PSXBus bus;
    cpu::CPU cpu(bus);
    bus.memory().write32(0x100u, 0u); // NOP in main RAM
    cpu.reset(0x100u);
    cpu.step();
    t.check(bus.cycles() == 6 && cpu.state().pc == 0x104u,
            "CPU instruction fetch uses the shared bus and advances time before instruction completion");
}

void test_cpu_memory_access_timing(Runner& t)
{
    bus::PSXBus bus;
    auto& memory = bus.memory();

    memory.write32(0x200u, 0x11223344u);
    const auto before_read = bus.cycles();
    t.check(bus.cpu_read32(0x200u) == 0x11223344u &&
            bus.cycles() - before_read == 5,
            "main-RAM data read consumes the documented five CPU cycles");

    const auto before_write = bus.cycles();
    bus.cpu_write32(0x204u, 0x55667788u);
    t.check(bus.cycles() - before_write == 1 && memory.read32(0x204u) == 0x55667788u,
            "main-RAM write consumes one CPU cycle at the W-buffer interface");

    const auto before_scratch = bus.cycles();
    bus.cpu_write32(0x1F800000u, 0xCAFEBABEu);
    const auto value = bus.cpu_read32(0x1F800000u);
    t.check(value == 0xCAFEBABEu && bus.cycles() - before_scratch == 2,
            "scratchpad read/write each consume one CPU cycle");
}

void test_cpu_integer_unit_timing(Runner& t)
{
    bus::PSXBus bus;
    cpu::CPU cpu(bus);
    cpu.reset(0x100u);
    bus.memory().write32(0x100u, 0x00220018u); // MULT r0, r1, r2
    cpu.step();
    t.check(bus.cycles() == 11,
            "MULT occupies six execution cycles in addition to its RAM fetch slot");

    bus.reset();
    cpu.reset(0x100u);
    bus.memory().write32(0x100u, 0x0022001Au); // DIV r0, r1, r2
    cpu.step();
    t.check(bus.cycles() == 41,
            "DIV occupies thirty-six execution cycles in addition to its RAM fetch slot");
}

void test_scheduled_dma_and_cpu_contention(Runner& t)
{
    bus::PSXBus bus;
    auto& memory = bus.memory();
    memory.write32(0x100, 0xA55A1234u);
    bus.set_dma_endpoint(0, { {}, [](core::u32) {} });
    memory.write32(0x1F8010F0u, 1u << 3);
    memory.write32(0x1F801080u, 0);
    memory.write32(0x1F801084u, 1);
    memory.write32(0x1F801088u, 1u | (1u << 24));

    unsigned event_count = 0;
    bus.scheduler().schedule_at(2, [&] { ++event_count; });
    t.check(bus.dma_owns_bus(), "DMA requests the shared bus before scheduled CPU access");
    const auto value = bus.cpu_read32(0x100);
    t.check(value == 0xA55A1234u && !bus.dma().busy(0),
            "CPU transaction waits for DMA ownership to end and then completes");
    bus.tick(2);
    t.check(event_count == 1 && bus.scheduler().now() == bus.cycles(),
            "peripheral event and DMA service remain aligned with bus time");
}
}


void test_gpu_busy_timing(Runner& t)
{
    bus::PSXBus bus;
    bus.memory().write32(0x1F801810u, 0x02000000u | (1u << 16) | 1u);
    bus.memory().write32(0x1F801810u, 0x00000000u);
    bus.memory().write32(0x1F801810u, 0x00000000u);
    t.check(bus.gpu().busy() && (bus.gpu().read_gp1() & (1u << 26)) == 0,
            "GPU keeps command-ready low while the deterministic raster cost is in flight");
    const auto before = bus.gpu().busy_cycles();
    bus.tick(before);
    t.check(!bus.gpu().busy() && (bus.gpu().read_gp1() & (1u << 26)) != 0,
            "GPU busy interval retires on the same master timeline as CPU/DMA/GTE");
}

void test_gpu_dma_status_semantics(Runner& t)
{
    bus::PSXBus bus;
    bus.memory().write32(0x1F801814u, 0x04000002u);
    t.check((bus.gpu().read_gp1() & (3u << 29)) == (2u << 29) &&
            (bus.gpu().read_gp1() & (1u << 25)) != 0,
            "GP1 DMA direction selects the documented write-FIFO-empty DREQ source");
    bus.memory().write32(0x1F801810u, 0x20000000u);
    bus.memory().write32(0x1F801810u, 0x00000000u);
    bus.memory().write32(0x1F801810u, 0x00010000u);
    bus.memory().write32(0x1F801810u, 0x00000001u);
    t.check((bus.gpu().read_gp1() & (1u << 28)) == 0,
            "GPU command-buffer ready status changes while a polygon is executing");
}
bool run_stage7_tests()
{
    Runner t;
    TestLog::instance() << "\n=== Stage 7: deterministic device timing ===\n";
    test_rational_clock_domain(t);
    test_video_timing(t);
    test_event_order_and_deadlines(t);
    test_cancel_and_reset(t);
    test_bus_clock_and_events(t);
    test_gte_uses_bus_timeline(t);
    test_cpu_instruction_fetch_timing(t);
    test_cpu_memory_access_timing(t);
    test_cpu_integer_unit_timing(t);
    test_scheduled_dma_and_cpu_contention(t);
    test_gpu_busy_timing(t);
    test_gpu_dma_status_semantics(t);
    TestLog::instance() << "Stage 7 result: " << t.passed << " passed, " << t.failed << " failed.\n";
    return t.failed == 0;
}
}
