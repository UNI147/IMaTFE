#pragma once

#include "Address.h"
#include "Types.h"

#include <array>
#include <cstddef>
#include <span>
#include <stdexcept>
#include <vector>
#include <functional>

namespace imatfe::core::psx
{

enum class MemoryRegion
{
    MainRam,
    Dev0,
    Scratchpad,
    Io,
    Dev8,
    Dev1,
    Bios,
    CacheControl,
    Unmapped,
};

struct ResolvedAddress
{
    MemoryRegion region = MemoryRegion::Unmapped;
    std::size_t offset = 0;
    bool cached = false;
};

class Memory final
{
public:
    using IoRead32 = std::function<u32(Address)>;
    using IoWrite32 = std::function<void(Address, u32)>;
    explicit Memory(std::size_t bios_size = BIOS_MAX_SIZE);

    u8  read8 (Address address) const;
    u16 read16(Address address) const;
    u32 read32(Address address) const;

    void write8 (Address address, u8 value);
    void write16(Address address, u16 value);
    void write32(Address address, u32 value);

    ResolvedAddress resolve(Address address) const noexcept;

    // Peripheral hooks let the CPU-visible I/O window be backed by actual
    // devices instead of a detached byte array.
    void set_io_callbacks(IoRead32 read32, IoWrite32 write32);
    void set_dma_irq(bool asserted) noexcept;
    u32 read_io_raw32(Address address) const noexcept;
    void write_io_raw32(Address address, u32 value) noexcept;

    std::span<const u8> main_ram() const noexcept { return main_ram_; }
    std::span<u8>       main_ram()       noexcept { return main_ram_; }

    std::span<u8> scratchpad() noexcept { return scratchpad_; }
    std::span<const u8> scratchpad() const noexcept { return scratchpad_; }

private:
    template <typename T>
    T read(Address address) const;

    template <typename T>
    void write(Address address, T value);

    static bool contains(PhysicalAddress address, Address base, std::size_t size) noexcept;

    std::vector<u8> main_ram_;
    std::vector<u8> dev0_;
    std::array<u8, SCRATCHPAD_SIZE> scratchpad_{};
    std::array<u8, IO_SIZE> io_{};
    std::array<u8, DEV8_SIZE> dev8_{};
    std::vector<u8> dev1_;
    std::vector<u8> bios_;
    std::array<u8, 0x100> cache_control_{};
    IoRead32 io_read32_;
    IoWrite32 io_write32_;
};

} // namespace imatfe::core::psx
