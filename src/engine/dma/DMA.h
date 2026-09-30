#pragma once

#include "engine/core/Types.h"

#include <array>
#include <functional>

namespace imatfe::dma
{

class DMA final
{
public:
    using Word = core::u32;
    using ReadRam = std::function<Word(Word)>;
    using WriteRam = std::function<void(Word, Word)>;
    using DeviceRead = std::function<Word()>;
    using DeviceWrite = std::function<void(Word)>;
    using IrqSink = std::function<void(bool)>;

    struct Endpoint
    {
        DeviceRead read;
        DeviceWrite write;
    };

    struct Channel
    {
        Word madr = 0;
        Word bcr = 0;
        Word chcr = 0;
    };

    explicit DMA(ReadRam read_ram, WriteRam write_ram);

    void reset() noexcept;

    Word read_register(Word address) const noexcept;
    void write_register(Word address, Word value);

    void set_endpoint(unsigned channel, Endpoint endpoint);
    void set_request(unsigned channel, bool asserted) noexcept;
    void set_irq_sink(IrqSink sink);

    // One tick consumes one granted main-bus word. Arbitration is performed
    // here, while the owner of the bus (CPU/DMA) is decided by PSXBus.
    void tick();
    void tick(unsigned bus_slots);

    bool busy(unsigned channel) const noexcept;
    bool any_active() const noexcept;
    bool bus_request_pending() const noexcept;
    void resume_after_cpu_slot() noexcept;
    bool irq() const noexcept { return irq_; }
    bool bus_error() const noexcept { return (dicr_ & (1u << 15)) != 0; }
    Word dpcr() const noexcept { return dpcr_; }
    Word dicr() const noexcept { return dicr_; }
    const Channel& channel(unsigned index) const noexcept { return channels_[index]; }

private:
    struct LinkedListState
    {
        Word next = 0;
        Word words_left = 0;
        bool active = false;
    };

    std::array<Channel, 7> channels_{};
    std::array<Endpoint, 7> endpoints_{};
    std::array<bool, 7> request_{};
    std::array<Word, 7> remaining_{};
    std::array<Word, 7> block_remaining_{};
    std::array<LinkedListState, 7> linked_{};
    std::array<unsigned, 7> chop_dma_left_{};

    Word dpcr_ = 0x07654321u;
    Word dicr_ = 0;
    bool irq_ = false;
    IrqSink irq_sink_;
    ReadRam read_ram_;
    WriteRam write_ram_;

    void start(unsigned channel);
    void transfer_word(unsigned channel);
    void finish(unsigned channel, bool interrupt_boundary = true);
    void block_boundary(unsigned channel);
    void raise_bus_error(unsigned channel) noexcept;
    void update_irq() noexcept;

    unsigned select_channel() const noexcept;
    bool eligible(unsigned channel) const noexcept;
    void begin_linked_list(unsigned channel);

    static Word count16(Word value) noexcept { return value ? value : 0x10000u; }
    static Word count8(Word value) noexcept { return value ? value : 0x100u; }
    static unsigned priority(Word dpcr, unsigned channel) noexcept
    {
        return (dpcr >> (channel * 4u)) & 7u;
    }
    static bool enabled(Word dpcr, unsigned channel) noexcept
    {
        return (dpcr & (1u << (channel * 4u + 3u))) != 0;
    }
    bool irq_per_boundary(unsigned channel) const noexcept;
    unsigned dma_window(unsigned channel) const noexcept;
};

} // namespace imatfe::dma
