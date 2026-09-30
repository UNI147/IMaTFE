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
    tpage_ = 0;
    texture_window_mask_x_ = 0;
    texture_window_mask_y_ = 0;
    texture_window_offset_x_ = 0;
    texture_window_offset_y_ = 0;
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

void GPU::clear_vram(Half color) noexcept
{
    std::fill(vram_.begin(), vram_.end(), color);
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

GPU::Half GPU::blend_semi_transparent(Half foreground, Half background) const noexcept
{
    const core::u32 mode = (tpage_ >> 5) & 3u;
    Half result = 0;
    for (unsigned shift : {0u, 5u, 10u})
    {
        const core::u32 src = (foreground >> shift) & 0x1Fu;
        const core::u32 dst = (background >> shift) & 0x1Fu;
        core::u32 value = 0;
        switch (mode)
        {
        case 0: value = (src + dst) >> 1; break;                 // 50% blend
        case 1: value = std::min(31u, src + dst); break;         // add
        case 2: value = dst > src ? dst - src : 0u; break;       // subtract
        case 3: value = std::min(31u, dst + (src >> 2)); break;  // add quarter
        }
        result = static_cast<Half>(result | (value << shift));
    }
    return result;
}

void GPU::plot_semi(core::s32 x, core::s32 y, Half color, bool enabled, bool stp) noexcept
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
    if (enabled && stp)
        color = blend_semi_transparent(color, dst);
    if (mask_set_)
        color = static_cast<Half>(color | 0x8000u);
    dst = color;
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
        packet_kind_ = PacketKind::Polygon;
        const std::size_t vertices = quad ? 4 : 3;
        // Polygon packet order is command, then for each vertex: XY, UV when
        // textured; Gouraud color for the next vertex precedes that vertex.
        packet_expected_ = textured
            ? 1 + vertices * 2 + (gouraud ? vertices - 1 : 0)
            : 1 + vertices + (gouraud ? vertices - 1 : 0);
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
        const Word size_code = (command >> 27) & 3u;
        packet_kind_ = PacketKind::Rectangle;
        // command + XY + optional UV + optional variable size
        packet_expected_ = textured
            ? (size_code == 0 ? 4 : 3)
            : (size_code == 0 ? 3 : 2);
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
        tpage_ = static_cast<core::u16>(command & 0x1FFFu);
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
    case 0xE2u:
        texture_window_mask_x_ = static_cast<core::u8>(command & 0x1Fu);
        texture_window_mask_y_ = static_cast<core::u8>((command >> 5) & 0x1Fu);
        texture_window_offset_x_ = static_cast<core::u8>((command >> 10) & 0x1Fu);
        texture_window_offset_y_ = static_cast<core::u8>((command >> 15) & 0x1Fu);
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
                {
                    // GP0(02h) writes VRAM directly: drawing-area offsets and
                    // mask-bit test/set do not affect the fill operation.
                    const core::s32 dst_x = x + xx + x0;
                    const core::s32 dst_y = y + y0;
                    if (dst_x < 0 || dst_y < 0 ||
                        dst_x >= static_cast<core::s32>(VRAM_WIDTH) ||
                        dst_y >= static_cast<core::s32>(VRAM_HEIGHT))
                        continue;
                    vram_[static_cast<std::size_t>(dst_y) * VRAM_WIDTH +
                          static_cast<std::size_t>(dst_x)] = pixel;
                }
}

GPU::TextureInfo GPU::decode_texture_info(core::u32 uv0, core::u32 uv1, bool raw) noexcept
{
    TextureInfo info{};
    info.clut = static_cast<core::u16>(uv0 >> 16);
    info.tpage = static_cast<core::u16>(uv1 >> 16);
    info.raw = raw;
    return info;
}

GPU::TexCoord GPU::apply_texture_window(TexCoord uv) const noexcept
{
    const core::u32 mask_x = static_cast<core::u32>(texture_window_mask_x_) << 3;
    const core::u32 mask_y = static_cast<core::u32>(texture_window_mask_y_) << 3;
    const core::u32 off_x = static_cast<core::u32>(texture_window_offset_x_) << 3;
    const core::u32 off_y = static_cast<core::u32>(texture_window_offset_y_) << 3;
    uv.u = static_cast<core::u8>((uv.u & ~mask_x) | (off_x & mask_x));
    uv.v = static_cast<core::u8>((uv.v & ~mask_y) | (off_y & mask_y));
    return uv;
}

core::u8 GPU::fetch_texture_index(TexCoord uv, const TextureInfo& info) const noexcept
{
    const core::u32 page_x = static_cast<core::u32>(info.tpage & 0xFu) * 64u;
    const core::u32 page_y = ((static_cast<core::u32>(info.tpage) >> 4) & 1u) * 256u;
    const core::u32 mode = (static_cast<core::u32>(info.tpage) >> 7) & 3u;
    const core::u32 y = page_y + uv.v;

    if (mode == 0) // 4-bit CLUT: four indices per VRAM halfword.
    {
        const core::u32 x = page_x + (static_cast<core::u32>(uv.u) >> 2);
        const Half packed = vram(x, y);
        const unsigned shift = (static_cast<unsigned>(uv.u) & 3u) * 4u;
        return static_cast<core::u8>((packed >> shift) & 0xFu);
    }

    if (mode == 1) // 8-bit CLUT: two indices per VRAM halfword.
    {
        const core::u32 x = page_x + (static_cast<core::u32>(uv.u) >> 1);
        const Half packed = vram(x, y);
        const unsigned shift = (static_cast<unsigned>(uv.u) & 1u) * 8u;
        return static_cast<core::u8>((packed >> shift) & 0xFFu);
    }

    return 0;
}

GPU::Half GPU::fetch_clut_color(core::u8 index, const TextureInfo& info) const noexcept
{
    // CBA addresses a palette row in VRAM in 16-halfword units.  The index
    // itself is a halfword offset into that row; it is not multiplied again.
    const core::u32 clut_x = (static_cast<core::u32>(info.clut) & 0x3Fu) * 16u;
    const core::u32 clut_y = (static_cast<core::u32>(info.clut) >> 6) & 0x1FFu;
    return vram(clut_x + static_cast<core::u32>(index), clut_y);
}

GPU::Half GPU::sample_texture(TexCoord uv, const TextureInfo& info) const noexcept
{
    uv = apply_texture_window(uv);

    const core::u32 page_x = static_cast<core::u32>(info.tpage & 0xFu) * 64u;
    const core::u32 page_y = ((static_cast<core::u32>(info.tpage) >> 4) & 1u) * 256u;
    const core::u32 mode = (static_cast<core::u32>(info.tpage) >> 7) & 3u;

    if (mode == 0 || mode == 1)
    {
        const core::u8 index = fetch_texture_index(uv, info);
        return fetch_clut_color(index, info);
    }

    // 15-bit direct texture: one texel per VRAM halfword.
    return vram(page_x + static_cast<core::u32>(uv.u), page_y + uv.v);
}

GPU::Half GPU::modulate_texture(Half texel, Word color) const noexcept
{
    if (texel == 0)
        return 0;
    const auto mod = [](core::u16 tex, core::u32 c, unsigned shift) -> core::u16 {
        const core::u32 t = (tex >> shift) & 0x1Fu;
        const core::u32 v = (c >> (shift == 0 ? 0 : shift == 5 ? 8 : 16)) & 0xFFu;
        return static_cast<core::u16>(std::min<core::u32>(31u, (t * v) / 128u));
    };
    const core::u16 r = mod(texel, color, 0);
    const core::u16 g = mod(texel, color, 5);
    const core::u16 b = mod(texel, color, 10);
    return static_cast<Half>(r | (g << 5) | (b << 10) | (texel & 0x8000u));
}

void GPU::raster_textured_triangle(Vertex a, Vertex b, Vertex c,
                                   TexCoord ua, TexCoord ub, TexCoord uc,
                                   Word ca, Word cb, Word cc,
                                   bool gouraud, bool semi, const TextureInfo& texture)
{
    const double area = edge(a, b, static_cast<double>(c.x), static_cast<double>(c.y));
    if (area == 0.0)
        return;

    const core::s32 min_x = std::max<core::s32>({
        static_cast<core::s32>(draw_left_) - draw_offset_x_, -draw_offset_x_,
        static_cast<core::s32>(std::min({a.x, b.x, c.x}))});
    const core::s32 max_x = std::min<core::s32>({
        static_cast<core::s32>(draw_right_) - draw_offset_x_,
        static_cast<core::s32>(VRAM_WIDTH - 1) - draw_offset_x_,
        static_cast<core::s32>(std::max({a.x, b.x, c.x}))});
    const core::s32 min_y = std::max<core::s32>({
        static_cast<core::s32>(draw_top_) - draw_offset_y_, -draw_offset_y_,
        static_cast<core::s32>(std::min({a.y, b.y, c.y}))});
    const core::s32 max_y = std::min<core::s32>({
        static_cast<core::s32>(draw_bottom_) - draw_offset_y_,
        static_cast<core::s32>(VRAM_HEIGHT - 1) - draw_offset_y_,
        static_cast<core::s32>(std::max({a.y, b.y, c.y}))});
    if (min_x > max_x || min_y > max_y)
        return;

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

            const auto interp_u = [&](core::u8 av, core::u8 bv, core::u8 cv) -> core::u8 {
                return static_cast<core::u8>(std::clamp(std::lround(av*w0 + bv*w1 + cv*w2), 0L, 255L));
            };
            const TexCoord uv{interp_u(ua.u, ub.u, uc.u), interp_u(ua.v, ub.v, uc.v)};
            const Half texel = sample_texture(uv, texture);
            if (texel == 0)
                continue; // texture color 0000h is fully transparent.

            Half out = texel;
            if (!texture.raw)
            {
                const auto interp_channel = [&](unsigned shift) -> Word {
                    const double value = ((ca >> shift) & 0xFFu) * w0 +
                                         ((cb >> shift) & 0xFFu) * w1 +
                                         ((cc >> shift) & 0xFFu) * w2;
                    return static_cast<Word>(std::clamp(static_cast<int>(std::lround(value)), 0, 255));
                };
                const Word color = gouraud
                    ? (interp_channel(0) | (interp_channel(8) << 8) | (interp_channel(16) << 16))
                    : ca;
                out = modulate_texture(texel, color);
            }
            plot_semi(x, y, out, semi, (texel & 0x8000u) != 0);
        }
    }
}

void GPU::raster_textured_rectangle(Vertex origin, TexCoord uv, core::s32 width, core::s32 height,
                                    const TextureInfo& texture, bool semi)
{
    for (core::s32 y = 0; y < height; ++y)
    {
        for (core::s32 x = 0; x < width; ++x)
        {
            TexCoord tc{static_cast<core::u8>(uv.u + x), static_cast<core::u8>(uv.v + y)};
            const Half texel = sample_texture(tc, texture);
            if (texel == 0)
                continue;
            Half out = texel;
            if (!texture.raw)
                out = modulate_texture(texel, rgb_components(packet_command_));
            plot_semi(origin.x + x, origin.y + y, out, semi, (texel & 0x8000u) != 0);
        }
    }
}

void GPU::raster_triangle(Vertex a, Vertex b, Vertex c, Word ca, Word cb, Word cc, bool gouraud, bool semi)
{
    const double area = edge(a, b, static_cast<double>(c.x), static_cast<double>(c.y));
    if (area == 0.0)
        return;

    // Restrict the candidate box before iterating. Coordinates are signed GP0
    // values; a malformed/off-screen primitive must not cause an enormous loop.
    const core::s32 min_x = std::max<core::s32>({
        static_cast<core::s32>(draw_left_) - draw_offset_x_,
        -draw_offset_x_, 0 - draw_offset_x_, std::min({a.x, b.x, c.x})});
    const core::s32 max_x = std::min<core::s32>({
        static_cast<core::s32>(draw_right_) - draw_offset_x_,
        static_cast<core::s32>(VRAM_WIDTH - 1) - draw_offset_x_,
        std::max({a.x, b.x, c.x})});
    const core::s32 min_y = std::max<core::s32>({
        static_cast<core::s32>(draw_top_) - draw_offset_y_,
        -draw_offset_y_, std::min({a.y, b.y, c.y})});
    const core::s32 max_y = std::min<core::s32>({
        static_cast<core::s32>(draw_bottom_) - draw_offset_y_,
        static_cast<core::s32>(VRAM_HEIGHT - 1) - draw_offset_y_,
        std::max({a.y, b.y, c.y})});
    if (min_x > max_x || min_y > max_y)
        return;

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
                plot_semi(x, y, interpolate_rgb(ca, cb, cc, w0, w1, w2), semi);
            else
                plot_semi(x, y, rgb24_to_bgr15(ca), semi);
        }
    }
}

void GPU::raster_line(Vertex a, Vertex b, Word ca, Word cb, bool gouraud)
{
    // Drawing-area coordinates are inclusive. An inverted area is empty.
    if (draw_left_ > draw_right_ || draw_top_ > draw_bottom_)
        return;

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

void GPU::raster_rectangle(Vertex origin, core::s32 width, core::s32 height, Word color, bool semi)
{
    for (core::s32 y = 0; y < height; ++y)
        for (core::s32 x = 0; x < width; ++x)
            plot_semi(origin.x + x, origin.y + y, rgb24_to_bgr15(color), semi);
}

void GPU::execute_gp0_polygon()
{
    const Word command = gp0_packet_[0];
    const bool quad = (command & (1u << 27)) != 0;
    const bool gouraud = (command & (1u << 28)) != 0;
    const bool textured = (command & (1u << 26)) != 0;
    const bool raw_texture = (command & (1u << 24)) != 0;
    const bool semi = (command & (1u << 25)) != 0;
    const std::size_t vertices = quad ? 4 : 3;

    std::array<Vertex, 4> v{};
    std::array<TexCoord, 4> uv{};
    std::array<Word, 4> c{};
    c[0] = rgb_components(command);

    TextureInfo texture{};
    std::size_t index = 1;
    for (std::size_t i = 0; i < vertices; ++i)
    {
        if (gouraud && i > 0)
            c[i] = rgb_components(gp0_packet_[index++]);
        v[i] = unpack_vertex(gp0_packet_[index++]);
        if (textured)
        {
            const Word uv_word = gp0_packet_[index++];
            uv[i] = {static_cast<core::u8>(uv_word & 0xFFu),
                     static_cast<core::u8>((uv_word >> 8) & 0xFFu)};
            if (i == 0)
                texture.clut = static_cast<core::u16>(uv_word >> 16);
            else if (i == 1)
                texture.tpage = static_cast<core::u16>(uv_word >> 16);
        }
    }

    if (!textured)
    {
        raster_triangle(v[0], v[1], v[2], c[0], c[1], c[2], gouraud, semi);
        if (quad)
            raster_triangle(v[1], v[2], v[3], c[1], c[2], c[3], gouraud, semi);
        return;
    }

    texture.raw = raw_texture;
    const core::u16 previous_tpage = tpage_;
    tpage_ = texture.tpage;
    raster_textured_triangle(v[0], v[1], v[2], uv[0], uv[1], uv[2],
                             c[0], c[1], c[2], gouraud, semi, texture);
    if (quad)
        raster_textured_triangle(v[1], v[2], v[3], uv[1], uv[2], uv[3],
                                 c[1], c[2], c[3], gouraud, semi, texture);
    tpage_ = previous_tpage;
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
    const bool textured = (command & (1u << 26)) != 0;
    const bool raw_texture = (command & (1u << 24)) != 0;
    const bool semi = (command & (1u << 25)) != 0;
    const Vertex origin = unpack_vertex(gp0_packet_[1]);
    core::s32 width = 0;
    core::s32 height = 0;
    if (size_code == 1) { width = 1; height = 1; }
    else if (size_code == 2) { width = 8; height = 8; }
    else if (size_code == 3) { width = 16; height = 16; }
    else
    {
        const Word size = textured ? gp0_packet_[3] : gp0_packet_[2];
        width = static_cast<core::s32>(size & 0x3FFu);
        height = static_cast<core::s32>((size >> 16) & 0x1FFu);
    }
    if (width <= 0 || height <= 0)
        return;

    if (!textured)
    {
        raster_rectangle(origin, width, height, rgb_components(command), semi);
        return;
    }

    const Word uv_word = gp0_packet_[2];
    TextureInfo texture{};
    texture.clut = static_cast<core::u16>(uv_word >> 16);
    texture.tpage = tpage_;
    texture.raw = raw_texture;
    const TexCoord uv{static_cast<core::u8>(uv_word & 0xFFu),
                      static_cast<core::u8>((uv_word >> 8) & 0xFFu)};
    raster_textured_rectangle(origin, uv, width, height, texture, semi);
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
    tpage_ = 0;
    texture_window_mask_x_ = 0;
    texture_window_mask_y_ = 0;
    texture_window_offset_x_ = 0;
    texture_window_offset_y_ = 0;
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
