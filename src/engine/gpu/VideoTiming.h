#pragma once
#include "engine/core/Types.h"

namespace imatfe::gpu
{
// Scan timing driven by the GPU/video clock. This is a discrete timing model,
// not a claim that the current renderer reproduces analog video output.
class VideoTiming final
{
public:
    enum class Standard { NTSC, PAL };
    struct Profile { core::u32 clocks_per_line; core::u32 lines_per_field; core::u32 active_clocks; core::u32 active_lines; };
    void reset() noexcept;
    void set_standard(Standard standard) noexcept;
    void tick(core::u64 video_clocks) noexcept;
    Standard standard() const noexcept { return standard_; }
    core::u32 line() const noexcept { return line_; }
    core::u32 clock_in_line() const noexcept { return clock_in_line_; }
    core::u64 fields() const noexcept { return fields_; }
    bool hblank() const noexcept { return clock_in_line_ >= profile().active_clocks; }
    bool vblank() const noexcept { return line_ >= profile().active_lines; }
    const Profile& profile() const noexcept;
private:
    Standard standard_ = Standard::NTSC;
    core::u32 line_ = 0;
    core::u32 clock_in_line_ = 0;
    core::u64 fields_ = 0;
};
}
