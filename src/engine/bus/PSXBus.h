#pragma once

#include "engine/core/Memory.h"
#include "engine/core/Clock.h"
#include "engine/core/EventScheduler.h"
#include "engine/dma/DMA.h"
#include "engine/gpu/GPU.h"
#include "engine/gpu/VideoTiming.h"
#include "engine/core/ClockDomain.h"
#include "engine/gte/GTE.h"

namespace imatfe::bus
{

class PSXBus final
{
public:
    PSXBus();

    core::psx::Memory& memory() noexcept { return memory_; }
    const core::psx::Memory& memory() const noexcept { return memory_; }
    gpu::GPU& gpu() noexcept { return gpu_; }
    const gpu::GPU& gpu() const noexcept { return gpu_; }
    const gpu::VideoTiming& video_timing() const noexcept { return video_timing_; }
    dma::DMA& dma() noexcept { return dma_; }
    const dma::DMA& dma() const noexcept { return dma_; }

    void reset() noexcept;

    // Attach the CPU-owned GTE to the bus master timeline. The bus is the
    // sole source of device ticks when this connection is active.
    void attach_gte(gte::GTE* gte) noexcept { gte_ = gte; }

    // Shared-bus CPU transactions. A RAM/I/O access is allowed only when the
    // DMA arbiter has yielded the bus. Cache/scratchpad/COP/GTE are outside
    // this bus transaction model and remain CPU-local.
    core::u8 cpu_read8(core::u32 address);
    core::u16 cpu_read16(core::u32 address);
    core::u32 cpu_read32(core::u32 address);
    void cpu_write8(core::u32 address, core::u8 value);
    void cpu_write16(core::u32 address, core::u16 value);
    void cpu_write32(core::u32 address, core::u32 value);

    // One system bus slot. DMA wins if a channel is requesting the bus;
    // otherwise the caller may issue a CPU transaction in the slot.
    bool dma_owns_bus() const noexcept;
    bool cpu_bus_available() const noexcept;
    void cpu_yield_after_dma_slot();

    // Advance the deterministic master timeline. One bus slot is one PSX
    // CPU clock in this first timing model; device-specific divisors are
    // applied by their clock domains as they are implemented.
    void tick();
    void tick(unsigned bus_slots);

    // Advance master time while the CPU owns the shared bus. DMA must not
    // transfer during this interval, but video/GTE/events continue to run.
    void tick_cpu_bus(unsigned cpu_bus_cycles);
    core::Clock::Cycle cycles() const noexcept { return clock_.now(); }
    core::EventScheduler& scheduler() noexcept { return scheduler_; }
    const core::EventScheduler& scheduler() const noexcept { return scheduler_; }

    core::u32 read_io32(core::u32 address) const noexcept;
    void write_io32(core::u32 address, core::u32 value);

    void set_dma_endpoint(unsigned channel, dma::DMA::Endpoint endpoint);

private:
    core::Clock clock_;
    core::ClockDomain video_clock_{11, 7};
    gpu::VideoTiming video_timing_;
    core::EventScheduler scheduler_;
    core::psx::Memory memory_;
    gpu::GPU gpu_;
    dma::DMA dma_;
    gte::GTE* gte_ = nullptr;

    void update_dma_requests() noexcept;
    void service_dma_slot();
    void advance_time(core::u64 master_cycles, bool service_dma);
    void open_cpu_window_after_dma() noexcept;

    // Execute one indivisible CPU bus transaction. Arbitration is resolved
    // before the transfer starts; the bus slot is charged only after the
    // memory-mapped side effect has completed, so DMA cannot split a transfer.
    static bool needs_main_bus(const core::psx::Memory& memory, core::u32 address) noexcept;

    static unsigned cpu_access_cycles(const core::psx::Memory& memory,
                                      core::u32 address, bool write) noexcept;

    template<class Transfer>
    auto cpu_transaction(core::u32 address, bool write, Transfer&& transfer)
        -> decltype(transfer())
    {
        const bool shared_bus = needs_main_bus(memory_, address);
        if (shared_bus)
            while (!cpu_bus_available()) tick();

        auto result = transfer();
        if (shared_bus)
        {
            // A CPU transaction is indivisible from the arbiter's point of
            // view, but it may occupy several consecutive CPU clocks. The
            // documented main-RAM timings are therefore charged as a single
            // ownership interval rather than as repeated arbitration points.
            tick_cpu_bus(cpu_access_cycles(memory_, address, write));
            open_cpu_window_after_dma();
        }
        else if (memory_.resolve(address).region == core::psx::MemoryRegion::Scratchpad)
        {
            // ScratchPad is CPU-local and not a DMA target, but it still
            // consumes one CPU clock according to the hardware manual. It
            // does not participate in shared-bus arbitration.
            tick(1);
        }
        return result;
    }
};

} // namespace imatfe::bus
