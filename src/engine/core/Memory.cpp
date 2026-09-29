#include "Memory.h"

#include <cstring>
#include <limits>

namespace imatfe::core::psx
{

Memory::Memory(std::size_t bios_size)
    : main_ram_(MAIN_RAM_SIZE),
      dev0_(DEV0_SIZE),
      dev1_(DEV1_SIZE),
      bios_(bios_size)
{
    if (bios_size > BIOS_MAX_SIZE)
        throw std::invalid_argument("PSX BIOS is larger than the supported 512 KiB mapping");
}

bool Memory::contains(PhysicalAddress address, Address base, std::size_t size) noexcept
{
    const u64 a = address;
    const u64 b = base;
    return a >= b && (a - b) < size;
}

ResolvedAddress Memory::resolve(Address address) const noexcept
{
    // KSEG2 is not a normal physical mirror on PSX. The documented CPU
    // control registers live at FFFE0000h in this region.
    if (address >= CACHE_CONTROL_BASE && address < CACHE_CONTROL_BASE + cache_control_.size())
        return {MemoryRegion::CacheControl, address - CACHE_CONTROL_BASE, false};

    if (is_kseg2(address))
        return {};

    const PhysicalAddress physical = translate_segment(address);
    const bool cached = is_kuseg(address) || is_kseg0(address);

    if (contains(physical, MAIN_RAM_BASE, main_ram_.size()))
        return {MemoryRegion::MainRam, physical - MAIN_RAM_BASE, cached};
    if (contains(physical, DEV0_BASE, dev0_.size()))
        return {MemoryRegion::Dev0, physical - DEV0_BASE, cached};
    if (contains(physical, SCRATCHPAD_BASE, scratchpad_.size()))
        return {MemoryRegion::Scratchpad, physical - SCRATCHPAD_BASE, cached};
    if (contains(physical, IO_BASE, io_.size()))
        return {MemoryRegion::Io, physical - IO_BASE, false};
    if (contains(physical, DEV8_BASE, dev8_.size()))
        return {MemoryRegion::Dev8, physical - DEV8_BASE, false};
    if (contains(physical, DEV1_BASE, dev1_.size()))
        return {MemoryRegion::Dev1, physical - DEV1_BASE, cached};
    if (contains(physical, BIOS_BASE, bios_.size()))
        return {MemoryRegion::Bios, physical - BIOS_BASE, false};

    return {};
}

template <typename T>
T Memory::read(Address address) const
{
    static_assert(std::is_unsigned_v<T>);
    if (!is_aligned(address, sizeof(T)))
        throw std::runtime_error("PSX unaligned memory read");

    const auto r = resolve(address);
    const auto load = [r](const auto& storage) -> T {
        const auto* p = storage.data() + r.offset;
        T value = 0;
        for (std::size_t i = 0; i < sizeof(T); ++i)
            value |= static_cast<T>(p[i]) << (i * 8); // PSX is little-endian.
        return value;
    };

    switch (r.region)
    {
    case MemoryRegion::MainRam:      return load(main_ram_);
    case MemoryRegion::Dev0:         return load(dev0_);
    case MemoryRegion::Scratchpad:   return load(scratchpad_);
    case MemoryRegion::Io:           return load(io_);
    case MemoryRegion::Dev8:         return load(dev8_);
    case MemoryRegion::Dev1:          return load(dev1_);
    case MemoryRegion::Bios:          return load(bios_);
    case MemoryRegion::CacheControl: return load(cache_control_);
    case MemoryRegion::Unmapped:      throw std::out_of_range("PSX unmapped memory read");
    }
    throw std::out_of_range("PSX invalid memory region");
}

template <typename T>
void Memory::write(Address address, T value)
{
    static_assert(std::is_unsigned_v<T>);
    if (!is_aligned(address, sizeof(T)))
        throw std::runtime_error("PSX unaligned memory write");

    const auto r = resolve(address);
    const auto store = [r, value](auto& storage) {
        auto* p = storage.data() + r.offset;
        for (std::size_t i = 0; i < sizeof(T); ++i)
            p[i] = static_cast<u8>(value >> (i * 8));
    };

    switch (r.region)
    {
    case MemoryRegion::MainRam:      store(main_ram_); break;
    case MemoryRegion::Dev0:         store(dev0_); break;
    case MemoryRegion::Scratchpad:   store(scratchpad_); break;
    case MemoryRegion::Io:           store(io_); break;
    case MemoryRegion::Dev8:         store(dev8_); break;
    case MemoryRegion::Dev1:          store(dev1_); break;
    case MemoryRegion::Bios:          throw std::runtime_error("PSX BIOS is read-only");
    case MemoryRegion::CacheControl: store(cache_control_); break;
    case MemoryRegion::Unmapped:     throw std::out_of_range("PSX unmapped memory write");
    }
}

u8 Memory::read8(Address address) const { return read<u8>(address); }
u16 Memory::read16(Address address) const { return read<u16>(address); }
u32 Memory::read32(Address address) const { return read<u32>(address); }
void Memory::write8(Address address, u8 value) { write<u8>(address, value); }
void Memory::write16(Address address, u16 value) { write<u16>(address, value); }
void Memory::write32(Address address, u32 value) { write<u32>(address, value); }

} // namespace imatfe::core::psx
