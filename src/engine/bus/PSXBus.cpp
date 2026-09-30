#include "PSXBus.h"

namespace imatfe::bus
{
namespace
{
constexpr core::u32 GPU_GP0 = 0x1F801810u;
constexpr core::u32 GPU_GP1 = 0x1F801814u;
constexpr core::u32 I_STAT  = 0x1F801070u;
constexpr core::u32 I_MASK  = 0x1F801074u;
}

PSXBus::PSXBus()
    : memory_(), gpu_(), dma_(
        [this](core::u32 address) -> core::u32 {
            return memory_.read32(address & 0x001FFFFFu);
        },
        [this](core::u32 address, core::u32 value) {
            memory_.write32(address & 0x001FFFFFu, value);
        })
{
    memory_.set_io_callbacks(
        [this](core::u32 address) { return read_io32(address); },
        [this](core::u32 address, core::u32 value) { write_io32(address, value); });

    // DMA2 is the GPU command/data channel. Other channels remain explicit
    // endpoints so MDEC/CDROM/SPU/PIO can be attached without changing DMA.
    dma_.set_endpoint(2, {
        [this]() { return gpu_.read_gp0(); },
        [this](core::u32 value) { gpu_.write_gp0(value); }
    });
    dma_.set_irq_sink([this](bool asserted) { memory_.set_dma_irq(asserted); });
    reset();
}

void PSXBus::reset() noexcept
{
    clock_.reset();
    video_clock_.reset();
    video_timing_.reset();
    scheduler_.reset();
    gpu_.reset();
    dma_.reset();
    update_dma_requests();
}

void PSXBus::set_dma_endpoint(unsigned channel, dma::DMA::Endpoint endpoint)
{
    dma_.set_endpoint(channel, std::move(endpoint));
}

core::u32 PSXBus::read_io32(core::u32 address) const noexcept
{
    switch (address)
    {
    case GPU_GP0: return gpu_.read_gp0();
    case GPU_GP1: return gpu_.read_gp1();
    case I_STAT:  return memory_.read_io_raw32(I_STAT);
    case I_MASK:  return memory_.read_io_raw32(I_MASK);
    default:
        if (address >= 0x1F801080u && address <= 0x1F8010F4u)
            return dma_.read_register(address);
        return memory_.read_io_raw32(address);
    }
}

void PSXBus::write_io32(core::u32 address, core::u32 value)
{
    switch (address)
    {
    case GPU_GP0:
        gpu_.write_gp0(value);
        break;
    case GPU_GP1:
        gpu_.write_gp1(value);
        if ((value >> 24) == 0x08u)
            video_timing_.set_standard((gpu_.display_mode() & 0x08u) ? gpu::VideoTiming::Standard::PAL : gpu::VideoTiming::Standard::NTSC);
        else if ((value >> 24) == 0x00u)
            video_timing_.set_standard(gpu::VideoTiming::Standard::NTSC);
        break;
    case I_STAT:
    {
        // I_STAT is write-zero-to-clear: ones preserve the corresponding bit.
        const core::u32 current = memory_.read_io_raw32(I_STAT);
        memory_.write_io_raw32(I_STAT, current & value);
        break;
    }
    case I_MASK:
        memory_.write_io_raw32(I_MASK, value & 0x7FFu);
        break;
    default:
        if (address >= 0x1F801080u && address <= 0x1F8010F4u)
        {
            dma_.write_register(address, value);
            update_dma_requests();
            return;
        }
        memory_.write_io_raw32(address, value);
        break;
    }
}

void PSXBus::update_dma_requests() noexcept
{
    const auto& ch2 = dma_.channel(2);
    const bool gpu_to_ram = (ch2.chcr & 1u) == 0;

    // DMA channel 2 is driven by the GPU's DREQ state.  The GPU status
    // register already encodes GP1(04h)'s selected request source; for the
    // reverse direction (GPU -> RAM) the request is the GP0 read-FIFO state.
    const core::u32 status = gpu_.read_gp1();
    const bool request = gpu_to_ram
        ? ((status & (1u << 27)) != 0)
        : gpu_.dma_request();
    dma_.set_request(2, request);
}

bool PSXBus::dma_owns_bus() const noexcept
{
    return dma_.bus_request_pending();
}

bool PSXBus::cpu_bus_available() const noexcept
{
    return !dma_.bus_request_pending();
}

void PSXBus::open_cpu_window_after_dma() noexcept
{
    dma_.resume_after_cpu_slot();
}

void PSXBus::service_dma_slot()
{
    update_dma_requests();
    dma_.tick();
    update_dma_requests();
}

bool PSXBus::needs_main_bus(const core::psx::Memory& memory, core::u32 address) noexcept
{
    const auto region = memory.resolve(address).region;
    return region == core::psx::MemoryRegion::MainRam ||
           region == core::psx::MemoryRegion::Io;
}

unsigned PSXBus::cpu_access_cycles(const core::psx::Memory& memory,
                                   core::u32 address, bool write) noexcept
{
    const auto region = memory.resolve(address).region;

    // PlayStation Hardware, table 2-5:
    //   Main Memory -> CPU : 5 cycles for one word read.
    //   CPU -> W Buffer   : 1 cycle for a write accepted by the buffer.
    // ScratchPad is CPU-local high-speed memory and is not on the shared
    // external bus; it is handled separately below.
    if (region == core::psx::MemoryRegion::MainRam)
        return write ? 1u : 5u;

    // I/O and other external regions do not have a timing table in the
    // supplied Hardware manual section. Keep the existing one-slot model
    // until device-specific timing is introduced rather than inventing it.
    return 1u;
}

core::u8 PSXBus::cpu_read8(core::u32 address)
{
    return cpu_transaction(address, false, [&] { return memory_.read8(address); });
}

core::u16 PSXBus::cpu_read16(core::u32 address)
{
    return cpu_transaction(address, false, [&] { return memory_.read16(address); });
}

core::u32 PSXBus::cpu_read32(core::u32 address)
{
    return cpu_transaction(address, false, [&] { return memory_.read32(address); });
}

void PSXBus::cpu_write8(core::u32 address, core::u8 value)
{
    cpu_transaction(address, true, [&] { memory_.write8(address, value); return 0; });
}

void PSXBus::cpu_write16(core::u32 address, core::u16 value)
{
    cpu_transaction(address, true, [&] { memory_.write16(address, value); return 0; });
}

void PSXBus::cpu_write32(core::u32 address, core::u32 value)
{
    cpu_transaction(address, true, [&] { memory_.write32(address, value); return 0; });
}

void PSXBus::cpu_yield_after_dma_slot()
{
    service_dma_slot();
}

void PSXBus::advance_time(core::u64 master_cycles, bool service_dma)
{
    for (core::u64 i = 0; i < master_cycles; ++i)
    {
        clock_.advance();
        video_timing_.tick(video_clock_.advance());
        scheduler_.run_until(clock_.now());
        if (gte_)
            gte_->tick();
        gpu_.tick();
        if (service_dma)
            service_dma_slot();
    }
}

void PSXBus::tick()
{
    // A normal system tick is an arbitration point: DMA may consume the
    // current shared-bus slot. CPU transactions use tick_cpu_bus() instead
    // so a multi-cycle RAM transfer remains one indivisible bus ownership
    // interval.
    advance_time(1, true);
}

void PSXBus::tick(unsigned bus_slots)
{
    while (bus_slots-- != 0)
        tick();
}

void PSXBus::tick_cpu_bus(unsigned cpu_bus_cycles)
{
    if (cpu_bus_cycles != 0)
        advance_time(cpu_bus_cycles, false);
}

} // namespace imatfe::bus
