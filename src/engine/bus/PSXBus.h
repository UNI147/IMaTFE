#pragma once

#include "engine/core/Memory.h"
#include "engine/dma/DMA.h"
#include "engine/gpu/GPU.h"

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
    dma::DMA& dma() noexcept { return dma_; }
    const dma::DMA& dma() const noexcept { return dma_; }

    void reset() noexcept;

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

    void tick();
    void tick(unsigned bus_slots);

    core::u32 read_io32(core::u32 address) const noexcept;
    void write_io32(core::u32 address, core::u32 value);

    void set_dma_endpoint(unsigned channel, dma::DMA::Endpoint endpoint);

private:
    core::psx::Memory memory_;
    gpu::GPU gpu_;
    dma::DMA dma_;

    void update_dma_requests() noexcept;
    void service_dma_slot();
    void open_cpu_window_after_dma() noexcept;
};

} // namespace imatfe::bus
