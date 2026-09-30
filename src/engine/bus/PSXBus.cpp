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
    const core::u32 status = gpu_.read_gp1();
    const bool request = gpu_to_ram
        ? ((status & (1u << 27)) != 0)
        : ((status & (1u << 25)) != 0 || (status & (1u << 28)) != 0);
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

namespace
{
    bool needs_main_bus(const core::psx::Memory& memory, core::u32 address) noexcept
    {
        const auto region = memory.resolve(address).region;
        return region == core::psx::MemoryRegion::MainRam ||
               region == core::psx::MemoryRegion::Io;
    }
}

core::u8 PSXBus::cpu_read8(core::u32 address)
{
    if (needs_main_bus(memory_, address))
        while (!cpu_bus_available()) service_dma_slot();
    const core::u8 value = memory_.read8(address);
    if (needs_main_bus(memory_, address)) open_cpu_window_after_dma();
    return value;
}

core::u16 PSXBus::cpu_read16(core::u32 address)
{
    if (needs_main_bus(memory_, address))
        while (!cpu_bus_available()) service_dma_slot();
    const core::u16 value = memory_.read16(address);
    if (needs_main_bus(memory_, address)) open_cpu_window_after_dma();
    return value;
}

core::u32 PSXBus::cpu_read32(core::u32 address)
{
    if (needs_main_bus(memory_, address))
        while (!cpu_bus_available()) service_dma_slot();
    const core::u32 value = memory_.read32(address);
    if (needs_main_bus(memory_, address)) open_cpu_window_after_dma();
    return value;
}

void PSXBus::cpu_write8(core::u32 address, core::u8 value)
{
    if (needs_main_bus(memory_, address))
        while (!cpu_bus_available()) service_dma_slot();
    memory_.write8(address, value);
    if (needs_main_bus(memory_, address)) open_cpu_window_after_dma();
}

void PSXBus::cpu_write16(core::u32 address, core::u16 value)
{
    if (needs_main_bus(memory_, address))
        while (!cpu_bus_available()) service_dma_slot();
    memory_.write16(address, value);
    if (needs_main_bus(memory_, address)) open_cpu_window_after_dma();
}

void PSXBus::cpu_write32(core::u32 address, core::u32 value)
{
    if (needs_main_bus(memory_, address))
        while (!cpu_bus_available()) service_dma_slot();
    memory_.write32(address, value);
    if (needs_main_bus(memory_, address)) open_cpu_window_after_dma();
}

void PSXBus::cpu_yield_after_dma_slot()
{
    service_dma_slot();
}

void PSXBus::tick()
{
    service_dma_slot();
}

void PSXBus::tick(unsigned bus_slots)
{
    while (bus_slots-- != 0)
        tick();
}

} // namespace imatfe::bus
