#include "VideoTiming.h"

namespace imatfe::gpu
{
namespace
{
// Values are explicit model parameters. Line/field totals follow the common
// non-interlaced timing profiles; interlace half-lines are not modeled yet.
constexpr VideoTiming::Profile ntsc{3412, 263, 2560, 240};
constexpr VideoTiming::Profile pal{3405, 314, 2560, 288};
}
const VideoTiming::Profile& VideoTiming::profile() const noexcept
{
    return standard_ == Standard::PAL ? pal : ntsc;
}
void VideoTiming::reset() noexcept { line_ = clock_in_line_ = 0; fields_ = 0; }
void VideoTiming::set_standard(Standard standard) noexcept
{
    if (standard_ == standard) return;
    standard_ = standard;
    reset();
}
void VideoTiming::tick(core::u64 clocks) noexcept
{
    const auto& p = profile();
    while (clocks--)
    {
        if (++clock_in_line_ >= p.clocks_per_line)
        {
            clock_in_line_ = 0;
            if (++line_ >= p.lines_per_field) { line_ = 0; ++fields_; }
        }
    }
}
}
