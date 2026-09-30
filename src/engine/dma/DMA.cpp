#include "DMA.h"

#include <limits>
#include <utility>

namespace imatfe::dma
{
namespace
{
constexpr DMA::Word MADR_BASE = 0x1F801080u;
constexpr DMA::Word DPCR      = 0x1F8010F0u;
constexpr DMA::Word DICR      = 0x1F8010F4u;
constexpr DMA::Word CHCR_DIR  = 1u << 0;
constexpr DMA::Word CHCR_STEP = 1u << 1;
constexpr DMA::Word CHCR_CHOP = 1u << 8;
constexpr DMA::Word CHCR_SYNC_MASK = 3u << 9;
constexpr DMA::Word CHCR_SYNC_SHIFT = 9;
constexpr DMA::Word CHCR_START = 1u << 24;
constexpr DMA::Word CHCR_FORCE = 1u << 28;
constexpr DMA::Word CHCR_PAUSE = 1u << 29;
constexpr DMA::Word CHCR_SNOOP = 1u << 30;
constexpr DMA::Word MAIN_RAM_SIZE = 0x00200000u;
constexpr DMA::Word DICR_BOUNDARY_MASK = 0x0000007Fu;
constexpr DMA::Word DICR_CHANNEL_MASK = 0x007F0000u;
constexpr DMA::Word DICR_MASTER_ENABLE = 1u << 23;
constexpr DMA::Word DICR_FLAGS = 0x7F000000u;
constexpr DMA::Word DICR_BUS_ERROR = 1u << 15;
}

DMA::DMA(ReadRam read_ram, WriteRam write_ram)
    : read_ram_(std::move(read_ram)), write_ram_(std::move(write_ram))
{
    reset();
}

void DMA::reset() noexcept
{
    channels_ = {};
    request_ = {};
    remaining_ = {};
    block_remaining_ = {};
    linked_ = {};
    chop_dma_left_ = {};
    dpcr_ = 0x07654321u;
    dicr_ = 0;
    irq_ = false;
    if (irq_sink_)
        irq_sink_(false);
}

bool DMA::busy(unsigned channel) const noexcept
{
    return channel < channels_.size() && (channels_[channel].chcr & CHCR_START) != 0;
}

bool DMA::any_active() const noexcept
{
    for (unsigned c = 0; c < channels_.size(); ++c)
        if (busy(c)) return true;
    return false;
}

bool DMA::bus_request_pending() const noexcept
{
    return select_channel() < channels_.size();
}

void DMA::resume_after_cpu_slot() noexcept
{
    unsigned selected = channels_.size();
    unsigned best = std::numeric_limits<unsigned>::max();
    for (unsigned c = 0; c < channels_.size(); ++c)
    {
        if (!busy(c) || (channels_[c].chcr & CHCR_PAUSE) == 0)
            continue;
        const unsigned p = priority(dpcr_, c);
        if (selected == channels_.size() || p < best || (p == best && c > selected))
        {
            selected = c;
            best = p;
        }
    }
    if (selected < channels_.size())
    {
        channels_[selected].chcr &= ~CHCR_PAUSE;
        chop_dma_left_[selected] = dma_window(selected);
    }
}

void DMA::set_endpoint(unsigned channel, Endpoint endpoint)
{
    endpoints_.at(channel) = std::move(endpoint);
}

void DMA::set_request(unsigned channel, bool asserted) noexcept
{
    if (channel < request_.size())
        request_[channel] = asserted;
}

void DMA::set_irq_sink(IrqSink sink)
{
    irq_sink_ = std::move(sink);
    if (irq_sink_)
        irq_sink_(irq_);
}

DMA::Word DMA::read_register(Word address) const noexcept
{
    if (address >= MADR_BASE && address < MADR_BASE + 7u * 0x10u)
    {
        const unsigned channel = (address - MADR_BASE) / 0x10u;
        const unsigned reg = (address - MADR_BASE) & 0x0Fu;
        if (reg == 0) return channels_[channel].madr;
        if (reg == 4) return channels_[channel].bcr;
        if (reg == 8) return channels_[channel].chcr;
    }
    if (address == DPCR) return dpcr_;
    if (address == DICR) return dicr_;
    return 0;
}

void DMA::write_register(Word address, Word value)
{
    if (address >= MADR_BASE && address < MADR_BASE + 7u * 0x10u)
    {
        const unsigned channel = (address - MADR_BASE) / 0x10u;
        const unsigned reg = (address - MADR_BASE) & 0x0Fu;
        auto& ch = channels_[channel];
        if (reg == 0)
            ch.madr = value & 0x00FFFFFFu;
        else if (reg == 4)
            ch.bcr = value;
        else if (reg == 8)
        {
            // A write with START begins a new transfer. While active, changing
            // control fields is observable; START remains the busy latch.
            const bool was_busy = busy(channel);
            ch.chcr = value;
            if (!was_busy && (value & CHCR_START))
                start(channel);
        }
        return;
    }

    if (address == DPCR)
    {
        dpcr_ = value;
        return;
    }

    if (address == DICR)
    {
        // 15 and 23..16 are control bits; 24..30 are W1C flags; 31 is derived.
        const Word writable = DICR_BOUNDARY_MASK | DICR_BUS_ERROR | DICR_CHANNEL_MASK | DICR_MASTER_ENABLE;
        dicr_ = (dicr_ & ~writable) | (value & writable);
        dicr_ &= ~(value & DICR_FLAGS);
        update_irq();
    }
}

void DMA::start(unsigned channel)
{
    auto& ch = channels_[channel];
    const unsigned mode = (ch.chcr & CHCR_SYNC_MASK) >> CHCR_SYNC_SHIFT;

    linked_[channel] = {};
    remaining_[channel] = 0;
    block_remaining_[channel] = 0;
    chop_dma_left_[channel] = 0;

    if (channel == 6)
    {
        // OTC is fixed to decrementing RAM addresses in hardware.
        ch.chcr |= CHCR_STEP;
        ch.chcr &= ~(CHCR_DIR | CHCR_CHOP | CHCR_SYNC_MASK | CHCR_PAUSE | CHCR_SNOOP);
        remaining_[channel] = count16(ch.bcr & 0xFFFFu);
        return;
    }

    switch (mode)
    {
    case 0:
        remaining_[channel] = count16(ch.bcr & 0xFFFFu);
        break;
    case 1:
        block_remaining_[channel] = count16(ch.bcr & 0xFFFFu);
        remaining_[channel] = block_remaining_[channel] * count16(ch.bcr >> 16);
        break;
    case 2:
        if (channel != 2 || (ch.chcr & CHCR_DIR) == 0)
        {
            ch.chcr &= ~CHCR_START;
            return;
        }
        begin_linked_list(channel);
        break;
    default:
        ch.chcr &= ~CHCR_START;
        return;
    }

    if (ch.chcr & CHCR_FORCE)
        ch.chcr &= ~CHCR_FORCE;
}

void DMA::begin_linked_list(unsigned channel)
{
    auto& ll = linked_[channel];
    const Word address = channels_[channel].madr & 0x00FFFFFFu;
    if (address >= MAIN_RAM_SIZE)
    {
        raise_bus_error(channel);
        return;
    }

    const Word header = read_ram_(address);
    ll.next = header & 0x00FFFFFFu;
    ll.words_left = count8(header >> 24);
    ll.active = true;
    channels_[channel].madr = (address + 4u) & 0x00FFFFFFu;
}

bool DMA::eligible(unsigned channel) const noexcept
{
    if (channel >= channels_.size() || !busy(channel) || !enabled(dpcr_, channel))
        return false;

    const auto& ch = channels_[channel];
    if (ch.chcr & CHCR_PAUSE)
        return false;

    const unsigned mode = (ch.chcr & CHCR_SYNC_MASK) >> CHCR_SYNC_SHIFT;
    if (mode == 0)
        return true;

    if (ch.chcr & CHCR_FORCE)
        return true;

    if (mode == 1)
        return request_[channel];
    if (mode == 2)
        return channel == 2 && request_[channel] && linked_[channel].active;
    return false;
}

unsigned DMA::select_channel() const noexcept
{
    unsigned selected = channels_.size();
    unsigned best = std::numeric_limits<unsigned>::max();

    for (unsigned c = 0; c < channels_.size(); ++c)
    {
        if (!eligible(c)) continue;
        const unsigned p = priority(dpcr_, c);
        if (selected == channels_.size() || p < best || (p == best && c > selected))
        {
            selected = c;
            best = p;
        }
    }
    return selected;
}

unsigned DMA::dma_window(unsigned channel) const noexcept
{
    if ((channels_[channel].chcr & CHCR_CHOP) == 0)
        return 0;
    const unsigned n = (channels_[channel].chcr >> 16) & 0x7u;
    return 1u << n;
}

bool DMA::irq_per_boundary(unsigned channel) const noexcept
{
    return (dicr_ & (1u << channel)) != 0;
}

void DMA::block_boundary(unsigned channel)
{
    if (irq_per_boundary(channel) &&
        (dicr_ & (1u << (16u + channel))) &&
        (dicr_ & DICR_MASTER_ENABLE))
    {
        dicr_ |= 1u << (24u + channel);
        update_irq();
    }
}

void DMA::finish(unsigned channel, bool interrupt_boundary)
{
    channels_[channel].chcr &= ~CHCR_START;
    linked_[channel] = {};
    remaining_[channel] = 0;
    block_remaining_[channel] = 0;

    if (interrupt_boundary &&
        (dicr_ & (1u << (16u + channel))) &&
        (dicr_ & DICR_MASTER_ENABLE))
    {
        dicr_ |= 1u << (24u + channel);
    }
    update_irq();
}

void DMA::raise_bus_error(unsigned channel) noexcept
{
    channels_[channel].chcr &= ~CHCR_START;
    remaining_[channel] = 0;
    block_remaining_[channel] = 0;
    linked_[channel] = {};
    dicr_ |= DICR_BUS_ERROR;
    update_irq();
}

void DMA::update_irq() noexcept
{
    const bool asserted = (dicr_ & DICR_BUS_ERROR) != 0 ||
        ((dicr_ & DICR_MASTER_ENABLE) != 0 &&
         (((dicr_ >> 24) & ((dicr_ >> 16) & 0x7Fu)) != 0));

    irq_ = asserted;
    if (asserted) dicr_ |= 1u << 31;
    else dicr_ &= ~(1u << 31);

    if (irq_sink_)
        irq_sink_(irq_);
}

void DMA::transfer_word(unsigned channel)
{
    auto& ch = channels_[channel];
    const unsigned mode = (ch.chcr & CHCR_SYNC_MASK) >> CHCR_SYNC_SHIFT;
    const bool ram_to_device = (ch.chcr & CHCR_DIR) != 0;
    const bool decrement = (ch.chcr & CHCR_STEP) != 0;

    if (channel == 6)
    {
        const Word address = ch.madr & 0x00FFFFFCu;
        if (address >= MAIN_RAM_SIZE)
        {
            raise_bus_error(channel);
            return;
        }
        const Word next = (remaining_[channel] <= 1) ? 0x00FFFFFFu : ((address - 4u) & 0x00FFFFFFu);
        write_ram_(address, next);
        ch.madr = (address - 4u) & 0x00FFFFFFu;
        if (remaining_[channel]) --remaining_[channel];
        if (!remaining_[channel]) finish(channel);
        return;
    }

    if (mode == 2)
    {
        if (channel != 2 || !ram_to_device || !linked_[channel].active)
        {
            ch.chcr &= ~CHCR_START;
            return;
        }

        const Word address = ch.madr & 0x00FFFFFCu;
        if (address >= MAIN_RAM_SIZE)
        {
            raise_bus_error(channel);
            return;
        }
        const Word value = read_ram_(address);
        if (endpoints_[channel].write)
            endpoints_[channel].write(value);

        ch.madr = (address + 4u) & 0x00FFFFFFu;
        --linked_[channel].words_left;

        if (linked_[channel].words_left == 0)
        {
            block_boundary(channel);
            if (linked_[channel].next == 0x00FFFFFFu)
                finish(channel, !irq_per_boundary(channel));
            else
            {
                ch.madr = linked_[channel].next & 0x00FFFFFFu;
                begin_linked_list(channel);
            }
        }
        return;
    }

    const Word address = ch.madr & 0x00FFFFFCu;
    if (address >= MAIN_RAM_SIZE)
    {
        raise_bus_error(channel);
        return;
    }

    if (ram_to_device)
    {
        const Word value = read_ram_(address);
        if (endpoints_[channel].write)
            endpoints_[channel].write(value);
    }
    else
    {
        const Word value = endpoints_[channel].read ? endpoints_[channel].read() : 0;
        write_ram_(address, value);
    }

    ch.madr = decrement
        ? ((address - 4u) & 0x00FFFFFFu)
        : ((address + 4u) & 0x00FFFFFFu);

    if (remaining_[channel]) --remaining_[channel];

    if (mode == 1 && block_remaining_[channel])
    {
        --block_remaining_[channel];
        if (block_remaining_[channel] == 0)
        {
            block_boundary(channel);
            block_remaining_[channel] = count16(ch.bcr & 0xFFFFu);
        }
    }

    if (!remaining_[channel])
        finish(channel, !((mode == 1 || mode == 2) && irq_per_boundary(channel)));
}

void DMA::tick()
{
    const unsigned channel = select_channel();
    if (channel >= channels_.size())
        return;

    transfer_word(channel);

    const unsigned window = dma_window(channel);
    if (window != 0)
    {
        if (chop_dma_left_[channel] == 0)
            chop_dma_left_[channel] = window;
        if (chop_dma_left_[channel] > 0)
            --chop_dma_left_[channel];
        if (chop_dma_left_[channel] == 0)
            channels_[channel].chcr |= CHCR_PAUSE;
    }
}

void DMA::tick(unsigned bus_slots)
{
    while (bus_slots-- != 0)
    {
        // A chopped channel yields after its DMA window. The CPU-side bus
        // clears PAUSE when it successfully acquires a CPU access slot.
        tick();
    }
}

} // namespace imatfe::dma
