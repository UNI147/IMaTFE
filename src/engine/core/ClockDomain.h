#pragma once
#include "Types.h"
namespace imatfe::core {
class ClockDomain { public: ClockDomain(u64 num=1,u64 den=1):num_(num),den_(den){} void reset() noexcept {ticks_=phase_=0;} u64 advance(u64 source=1) noexcept { phase_ += source*num_; const u64 n=phase_/den_; phase_%=den_; ticks_+=n; return n;} u64 ticks()const noexcept{return ticks_;} u64 phase()const noexcept{return phase_;} private:u64 num_,den_,ticks_=0,phase_=0;};
}
