#pragma once

#include "Types.h"

namespace imatfe::core
{
// Integer master-clock timeline. Device clocks are expressed as ratios to the
// master clock; no wall-clock time or floating point is involved.
class Clock final
{
public:
    using Cycle = u64;
    void reset() noexcept { cycles_ = 0; }
    Cycle now() const noexcept { return cycles_; }
    void advance(Cycle cycles = 1) noexcept { cycles_ += cycles; }
private:
    Cycle cycles_ = 0;
};
}
