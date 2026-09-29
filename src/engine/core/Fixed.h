#pragma once

#include "Types.h"

#include <cassert>
#include <cmath>
#include <concepts>
#include <limits>
#include <type_traits>
#include <compare>

namespace imatfe::core
{

// Signed fixed-point value with FractionBits fractional bits.
// Storage is intentionally an integer: this mirrors the integer arithmetic
// used by the PSX/GTE instead of introducing host floating-point semantics.
template <std::signed_integral Storage, unsigned FractionBits>
class Fixed
{
public:
    using storage_type = Storage;
    static constexpr unsigned fraction_bits = FractionBits;
    static constexpr Storage scale = static_cast<Storage>(Storage{1} << FractionBits);

    constexpr Fixed() = default;

    static constexpr Fixed from_raw(Storage raw) noexcept
    {
        Fixed result;
        result.raw_ = raw;
        return result;
    }

    template <std::integral T>
    static constexpr Fixed from_integer(T value) noexcept
    {
        using Wide = s64;
        const Wide raw = static_cast<Wide>(value) * static_cast<Wide>(scale);
        return from_raw(static_cast<Storage>(raw));
    }

    static Fixed from_float(double value) noexcept
    {
        return from_raw(static_cast<Storage>(std::llround(value * static_cast<double>(scale))));
    }

    constexpr Storage raw() const noexcept { return raw_; }

    constexpr double to_double() const noexcept
    {
        return static_cast<double>(raw_) / static_cast<double>(scale);
    }

    constexpr Fixed operator+ (Fixed rhs) const noexcept { return from_raw(static_cast<Storage>(raw_ + rhs.raw_)); }
    constexpr Fixed operator- (Fixed rhs) const noexcept { return from_raw(static_cast<Storage>(raw_ - rhs.raw_)); }
    constexpr Fixed operator- () const noexcept { return from_raw(static_cast<Storage>(-raw_)); }

    constexpr Fixed& operator+=(Fixed rhs) noexcept { raw_ = static_cast<Storage>(raw_ + rhs.raw_); return *this; }
    constexpr Fixed& operator-=(Fixed rhs) noexcept { raw_ = static_cast<Storage>(raw_ - rhs.raw_); return *this; }

    constexpr Fixed operator*(Fixed rhs) const noexcept
    {
        using Wide = s64;
        const Wide product = static_cast<Wide>(raw_) * static_cast<Wide>(rhs.raw_);
        return from_raw(static_cast<Storage>(product >> FractionBits));
    }

    constexpr Fixed operator/(Fixed rhs) const noexcept
    {
        using Wide = s64;
        const Wide numerator = static_cast<Wide>(raw_) << FractionBits;
        return from_raw(static_cast<Storage>(numerator / static_cast<Wide>(rhs.raw_)));
    }

    constexpr Fixed& operator*=(Fixed rhs) noexcept { *this = *this * rhs; return *this; }
    constexpr Fixed& operator/=(Fixed rhs) noexcept { *this = *this / rhs; return *this; }

    constexpr auto operator<=>(const Fixed&) const noexcept = default;

private:
    Storage raw_ = 0;
};

// PSX/GTE matrix/vector values are commonly represented with 12 fractional
// bits. Keep the format explicit instead of pretending there is one universal
// "PSX float" type.
using Fixed12 = Fixed<s32, 12>;
using Fixed16 = Fixed<s32, 16>;
using Fixed8  = Fixed<s32, 8>;

} // namespace imatfe::core
