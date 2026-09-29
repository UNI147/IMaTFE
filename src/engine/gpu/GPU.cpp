#include "GPU.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace imatfe::gpu
{
namespace
{

inline double edge(const GPU::Vertex& a, const GPU::Vertex& b, double x, double y)
{
    return (x - a.x) * static_cast<double>(b.y - a.y) -
           (y - a.y) * static_cast<double>(b.x - a.x);
}
}

GPU::GPU() : vram_(VRAM_WIDTH * VRAM_HEIGHT)
{
    reset();
}

void GPU::reset() noexcept
{
    std::fill(vram_.begin(), vram_.end(), 0);
    status_ = 0x14802000u;
    display_enabled_ = true;
    display_x_ = display_y_ = 0;
    display_x1_ = 0x200;
    display_x2_ = 0xC00;
    display_y1_ = 0x10;
    display_y2_ = 0x100;
    display_mode_ = 0;
    draw_left_ = 0;
    draw_top_ = 0;
    draw_right_ = 1023;
    draw_bottom_ = 511;
    draw_offset_x_ = 0;
    draw_offset_y_ = 0;
    mask_set_ = false;
    mask_check_ = false;
    reset_packet();
}

void GPU::reset_packet() noexcept
{
    packet_size_ = 0;
    packet_expected_ = 0;
    packet_kind_ = PacketKind::None;
    packet_command_ = 0;
}

GPU::Half GPU::vram(std::size_t x, std::size_t y) const noexcept
{
    if (x >= VRAM_WIDTH || y >= VRAM_HEIGHT)
        return 0;
    return vram_[y * VRAM_WIDTH + x];
}

void GPU::set_vram(std::size_t x, std::size_t y, Half value) noexcept
{
    if (x < VRAM_WIDTH && y < VRAM_HEIGHT)
        vram_[y * VRAM_WIDTH + x] = value;
}

core::s16 GPU::signed16(Word value) noexcept
{
    return static_cast<core::s16>(value & 0xFFFFu);
}

GPU::Vertex GPU::unpack_vertex(Word value) noexcept
{
    return {signed16(value), signed16(value >> 16)};
}

GPU::Half GPU::rgb24_to_bgr15(Word value) noexcept
{
    const Half r = static_cast<Half>(value & 0xFFu);
    const Half g = static_cast<Half>((value >> 8) & 0xFFu);
    const Half b = static_cast<Half>((value >> 16) & 0xFFu);
    return static_cast<Half>((r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10));
}

GPU::Word GPU::rgb_components(Word value) noexcept
{
    return value & 0x00FFFFFFu;
}

GPU::Half GPU::interpolate_rgb(Word a, Word b, Word c, double wa, double wb, double wc) noexcept
{
    const auto channel = [=](int shift) {
        const double v = ((a >> shift) & 0xFFu) * wa +
                         ((b >> shift) & 0xFFu) * wb +
                         ((c >> shift) & 0xFFu) * wc;
        return static_cast<int>(std::clamp(std::lround(v), 0L, 255L));
    };
    return rgb24_to_bgr15(static_cast<Word>(channel(0) |
                                             (channel(8) << 8) |
                                             (channel(16) << 16)));
}

void GPU::plot(core::s32 x, core::s32 y, Half color) noexcept
{
    x += draw_offset_x_;
    y += draw_offset_y_;
    if (x < static_cast<core::s32>(draw_left_) || x > static_cast<core::s32>(draw_right_) ||
        y < static_cast<core::s32>(draw_top_) || y > static_cast<core::s32>(draw_bottom_) ||
        x < 0 || y < 0 || x >= static_cast<core::s32>(VRAM_WIDTH) || y >= static_cast<core::s32>(VRAM_HEIGHT))
        return;

    Half& dst = vram_[static_cast<std::size_t>(y) * VRAM_WIDTH + static_cast<std::size_t>(x)];
    if (mask_check_ && (dst & 0x8000u) != 0)
        return;
    if (mask_set_)
        color = static_cast<Half>(color | 0x8000u);
    dst = color;
}

void GPU::plot_rgb(core::s32 x, core::s32 y, Word color) noexcept
{
    plot(x, y, rgb24_to_bgr15(color));
}

void GPU::write_gp0(Word value)
{
    if (packet_kind_ == PacketKind::None)
    {
        begin_gp0(value);
        return;
    }

    consume_packet_word(value);
}

void GPU::begin_gp0(Word command)
{
    const Word opcode = command >> 24;
    packet_command_ = command;
    gp0_packet_[0] = command;
    packet_size_ = 1;
    packet_expected_ = 0;
    packet_kind_ = PacketKind::None;

    if (opcode == 0x02u)
    {
        packet_kind_ = PacketKind::QuickFill;
        packet_expected_ = 3;
        return;
    }

    if (opcode == 0x00u || opcode == 0x01u || opcode == 0x03u ||
        (opcode >= 0x04u && opcode <= 0x1Eu))
    {
        reset_packet();
        return;
    }

    const Word group = command >> 29;
    if (group == 1u)
    {
        const bool quad = (command & (1u << 27)) != 0;
        const bool gouraud = (command & (1u << 28)) != 0;
        const bool textured = (command & (1u << 26)) != 0;
        if (textured)
        {
            // Texture sampling is deliberately outside Stage 4; reject the packet
            // rather than pretending that a flat-color path is equivalent.
            reset_packet();
            return;
        }
        packet_kind_ = PacketKind::Polygon;
        const std::size_t vertices = quad ? 4 : 3;
        packet_expected_ = 1 + vertices + (gouraud ? vertices - 1 : 0);
        return;
    }

    if (group == 2u)
    {
        const bool polyline = (command & (1u << 27)) != 0;
        const bool gouraud = (command & (1u << 28)) != 0;
        if (polyline)
        {
            reset_packet();
            return;
        }
        packet_kind_ = PacketKind::Line;
        packet_expected_ = gouraud ? 4 : 3;
        return;
    }

    if (group == 3u)
    {
        const bool textured = (command & (1u << 26)) != 0;
        if (textured)
        {
            reset_packet();
            return;
        }
        const Word size_code = (command >> 27) & 3u;
        packet_kind_ = PacketKind::Rectangle;
        packet_expected_ = size_code == 0 ? 3 : 2;
        return;
    }

    if (group == 7u)
    {
        execute_gp0_environment(command);
        reset_packet();
        return;
    }

    reset_packet();
}

void GPU::consume_packet_word(Word value)
{
    if (packet_size_ >= gp0_packet_.size())
    {
        reset_packet();
        return;
    }
    gp0_packet_[packet_size_++] = value;
    if (packet_size_ >= packet_expected_)
    {
        execute_packet();
        reset_packet();
    }
}

void GPU::execute_packet()
{
    switch (packet_kind_)
    {
    case PacketKind::QuickFill: execute_gp0_quick_fill(); break;
    case PacketKind::Polygon:  execute_gp0_polygon(); break;
    case PacketKind::Line:     execute_gp0_line(); break;
    case PacketKind::Rectangle: execute_gp0_rectangle(); break;
    case PacketKind::None: break;
    }
}

void GPU::execute_gp0_environment(Word command)
{
    const Word opcode = command >> 24;
    switch (opcode)
    {
    case 0xE1u:
        status_ = (status_ & 0xFFFFE000u) | (command & 0x1FFFu);
        break;
    case 0xE3u:
        draw_left_ = command & 0x03FFu;
        draw_top_ = (command >> 10) & 0x01FFu;
        break;
    case 0xE4u:
        draw_right_ = command & 0x03FFu;
        draw_bottom_ = (command >> 10) & 0x01FFu;
        break;
    case 0xE5u:
        draw_offset_x_ = static_cast<core::s32>(command << 21) >> 21;
        draw_offset_y_ = static_cast<core::s32>(command << 10) >> 21;
        break;
    case 0xE6u:
        mask_set_ = (command & 1u) != 0;
        mask_check_ = (command & 2u) != 0;
        break;
    default:
        break;
    }
}

void GPU::execute_gp0_quick_fill()
{
    const Word color = gp0_packet_[0];
    const Word origin = gp0_packet_[1];
    const Word size = gp0_packet_[2];
    const core::s32 x0 = static_cast<core::s32>((origin & 0x3FFu) << 4);
    const core::s32 y0 = static_cast<core::s32>((origin >> 16) & 0x1FFu);
    const core::s32 width = static_cast<core::s32>((size & 0x3FFu) << 4);
    const core::s32 height = static_cast<core::s32>((size >> 16) & 0x1FFu);
    const Half pixel = rgb24_to_bgr15(color);
    for (core::s32 y = 0; y < height; ++y)
        for (core::s32 x = 0; x < width; ++x)
            if ((x & 0xF) == 0)
                for (core::s32 xx = 0; xx < 16 && x + xx < width; ++xx)
                    plot(x + xx + x0, y + y0, pixel);
}

void GPU::raster_triangle(Vertex a, Vertex b, Vertex c, Word ca, Word cb, Word cc, bool gouraud)
{
    const double area = edge(a, b, static_cast<double>(c.x), static_cast<double>(c.y));
    if (area == 0.0)
        return;

    const core::s32 min_x = std::max<core::s32>(static_cast<core::s32>(draw_left_) - draw_offset_x_,
        std::min({a.x, b.x, c.x}));
    const core::s32 max_x = std::min<core::s32>(static_cast<core::s32>(draw_right_) - draw_offset_x_,
        std::max({a.x, b.x, c.x}));
    const core::s32 min_y = std::max<core::s32>(static_cast<core::s32>(draw_top_) - draw_offset_y_,
        std::min({a.y, b.y, c.y}));
    const core::s32 max_y = std::min<core::s32>(static_cast<core::s32>(draw_bottom_) - draw_offset_y_,
        std::max({a.y, b.y, c.y}));

    for (core::s32 y = min_y; y <= max_y; ++y)
    {
        for (core::s32 x = min_x; x <= max_x; ++x)
        {
            const double px = x + 0.5;
            const double py = y + 0.5;
            const double w0 = edge(b, c, px, py) / area;
            const double w1 = edge(c, a, px, py) / area;
            const double w2 = edge(a, b, px, py) / area;
            if (w0 < 0.0 || w1 < 0.0 || w2 < 0.0)
                continue;
            if (gouraud)
                plot(x, y, interpolate_rgb(ca, cb, cc, w0, w1, w2));
            else
                plot_rgb(x, y, ca);
        }
    }
}

void GPU::raster_line(Vertex a, Vertex b, Word ca, Word cb, bool gouraud)
{
    const core::s32 dx = std::abs(b.x - a.x);
    const core::s32 sx = a.x < b.x ? 1 : -1;
    const core::s32 dy = -std::abs(b.y - a.y);
    const core::s32 sy = a.y < b.y ? 1 : -1;
    core::s32 err = dx + dy;
    const core::s32 steps = std::max(dx, -dy);
    core::s32 step = 0;
    core::s32 x = a.x;
    core::s32 y = a.y;
    for (;;)
    {
        if (gouraud && steps > 0)
        {
            const double t = static_cast<double>(step) / steps;
            const auto lerp = [t](Word a, Word b, unsigned shift) -> Word {
                const double av = static_cast<double>((a >> shift) & 0xFFu);
                const double bv = static_cast<double>((b >> shift) & 0xFFu);
                return static_cast<Word>(std::clamp(static_cast<int>(std::lround(av * (1.0 - t) + bv * t)), 0, 255));
            };
            const Word color = lerp(ca, cb, 0) |
                               (lerp(ca, cb, 8) << 8) |
                               (lerp(ca, cb, 16) << 16);
            plot_rgb(x, y, color);
        }
        else
            plot_rgb(x, y, ca);
        if (x == b.x && y == b.y)
            break;
        const core::s32 e2 = 2 * err;
        if (e2 >= dy) { err += dy; x += sx; }
        if (e2 <= dx) { err += dx; y += sy; }
        ++step;
    }
}

void GPU::raster_rectangle(Vertex origin, core::s32 width, core::s32 height, Word color)
{
    for (core::s32 y = 0; y < height; ++y)
        for (core::s32 x = 0; x < width; ++x)
            plot_rgb(origin.x + x, origin.y + y, color);
}

void GPU::execute_gp0_polygon()
{
    const Word command = gp0_packet_[0];
    const bool quad = (command & (1u << 27)) != 0;
    const bool gouraud = (command & (1u << 28)) != 0;
    const std::size_t vertices = quad ? 4 : 3;

    std::array<Vertex, 4> v{};
    std::array<Word, 4> c{};
    c[0] = rgb_components(command);
    std::size_t index = 1;
    for (std::size_t i = 0; i < vertices; ++i)
    {
        if (gouraud && i > 0)
            c[i] = rgb_components(gp0_packet_[index++]);
        v[i] = unpack_vertex(gp0_packet_[index++]);
    }
    raster_triangle(v[0], v[1], v[2], c[0], c[1], c[2], gouraud);
    if (quad)
        raster_triangle(v[1], v[2], v[3], c[1], c[2], c[3], gouraud);
}

void GPU::execute_gp0_line()
{
    const Word command = gp0_packet_[0];
    const bool gouraud = (command & (1u << 28)) != 0;
    const Word c0 = rgb_components(command);
    Vertex a = unpack_vertex(gp0_packet_[1]);
    Vertex b = unpack_vertex(gp0_packet_[2]);
    Word c1 = c0;
    if (gouraud)
    {
        a = unpack_vertex(gp0_packet_[1]);
        c1 = rgb_components(gp0_packet_[2]);
        b = unpack_vertex(gp0_packet_[3]);
    }
    raster_line(a, b, c0, c1, gouraud);
}

void GPU::execute_gp0_rectangle()
{
    const Word command = gp0_packet_[0];
    const Word size_code = (command >> 27) & 3u;
    const Vertex origin = unpack_vertex(gp0_packet_[1]);
    core::s32 width = 0;
    core::s32 height = 0;
    if (size_code == 1) { width = 1; height = 1; }
    else if (size_code == 2) { width = 8; height = 8; }
    else if (size_code == 3) { width = 16; height = 16; }
    else
    {
        const Word size = gp0_packet_[2];
        width = static_cast<core::s32>(size & 0x3FFu);
        height = static_cast<core::s32>((size >> 16) & 0x1FFu);
    }
    if (width > 0 && height > 0)
        raster_rectangle(origin, width, height, rgb_components(command));
}

void GPU::write_gp1(Word value)
{
    gp1_command(value);
}

void GPU::gp1_reset() noexcept
{
    status_ = 0x14802000u;
    display_enabled_ = true;
    display_x_ = display_y_ = 0;
    display_x1_ = 0x200;
    display_x2_ = 0xC00;
    display_y1_ = 0x10;
    display_y2_ = 0x100;
    display_mode_ = 0;
    draw_left_ = 0;
    draw_top_ = 0;
    draw_right_ = 1023;
    draw_bottom_ = 511;
    draw_offset_x_ = 0;
    draw_offset_y_ = 0;
    mask_set_ = false;
    mask_check_ = false;
    reset_packet();
}

void GPU::gp1_command(Word value) noexcept
{
    const Word opcode = value >> 24;
    switch (opcode)
    {
    case 0x00u: gp1_reset(); break;
    case 0x01u: reset_packet(); break;
    case 0x02u: status_ &= ~(1u << 24); break;
    case 0x03u: display_enabled_ = (value & 1u) == 0; break;
    case 0x05u:
        display_x_ = value & 0x3FFu;
        display_y_ = (value >> 10) & 0x1FFu;
        break;
    case 0x06u:
        display_x1_ = value & 0xFFFu;
        display_x2_ = (value >> 12) & 0xFFFu;
        break;
    case 0x07u:
        display_y1_ = value & 0x3FFu;
        display_y2_ = (value >> 10) & 0x3FFu;
        break;
    case 0x08u:
        display_mode_ = value & 0xFFu;
        break;
    case 0x09u:
        // Retail PSX has 1 MB VRAM; the 2 MB mode is not needed by Stage 4.
        break;
    default:
        break;
    }
}

GPU::Word GPU::read_gp0()
{
    return 0;
}

GPU::Word GPU::read_gp1() const noexcept
{
    return status_;
}

} // namespace imatfe::gpu
