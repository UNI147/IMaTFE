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

inline core::s32 sign_extend_11(core::u32 value) noexcept
{
    const core::u32 bits = value & 0x7FFu;
    return (bits & 0x400u) ? static_cast<core::s32>(bits | 0xFFFFF800u)
                           : static_cast<core::s32>(bits);
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
    dma_direction_ = 0;
    gpu_irq_ = false;
    draw_left_ = 0;
    draw_top_ = 0;
    draw_right_ = 1023;
    draw_bottom_ = 511;
    draw_offset_x_ = 0;
    draw_offset_y_ = 0;
    mask_set_ = false;
    mask_check_ = false;
    dither_enabled_ = false;
    draw_to_display_area_ = false;
    gpu_busy_cycles_ = 0;
    transfer_x_ = transfer_y_ = transfer_width_ = transfer_height_ = 0;
    transfer_source_x_ = transfer_source_y_ = 0;
    transfer_destination_x_ = transfer_destination_y_ = 0;
    transfer_words_remaining_ = transfer_pixel_index_ = 0;
    read_transfer_pixel_index_ = 0;
    vram_to_cpu_ = false;
    gp0_read_latch_ = 0;
    tpage_ = 0;
    texture_window_mask_x_ = 0;
    texture_window_mask_y_ = 0;
    texture_window_offset_x_ = 0;
    texture_window_offset_y_ = 0;
    reset_packet();
}

void GPU::refresh_ready_status() const noexcept
{
    // GPUSTAT ready/DREQ bits are state-derived.  The source describes bit 26
    // as "ready for a command word" and bit 28 as the write-FIFO-empty /
    // execution-ready indication.  The compact command parser has no separate
    // hardware FIFO, so an incomplete packet represents the parameter phase.
    status_ &= ~((1u << 24) | (1u << 25) | (1u << 26) | (1u << 27) | (1u << 28) | (3u << 29));
    if (gpu_irq_)
        status_ |= 1u << 24;

    const bool command_ready = packet_kind_ == PacketKind::None && gpu_busy_cycles_ == 0;
    const bool fifo_empty = command_ready;
    const bool read_fifo_ready = vram_to_cpu_ && read_transfer_pixel_index_ <
        static_cast<core::u32>(transfer_width_) * transfer_height_;

    if (command_ready) status_ |= 1u << 26;
    if (fifo_empty) status_ |= 1u << 28;
    if (read_fifo_ready) status_ |= 1u << 27;

    // GP1(04h): 0=off, 1=write FIFO not full, 2=write FIFO empty,
    // 3=read FIFO ready. DREQ is the selected ready condition.
    bool dreq = false;
    switch (dma_direction_ & 3u)
    {
    case 1: dreq = true; break; // FIFO is modeled as never full.
    case 2: dreq = fifo_empty; break;
    case 3: dreq = read_fifo_ready; break;
    default: break;
    }
    if (dreq) status_ |= 1u << 25;
    status_ |= static_cast<Word>(dma_direction_ & 3u) << 29;
}

void GPU::reset_packet() noexcept
{
    packet_size_ = 0;
    packet_expected_ = 0;
    packet_kind_ = PacketKind::None;
    packet_command_ = 0;
    packet_polyline_ = false;
    refresh_ready_status();
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
    return {sign_extend_11(value), sign_extend_11(value >> 16)};
}

GPU::Half GPU::rgb24_to_bgr15(Word value) noexcept
{
    const Half r = static_cast<Half>(value & 0xFFu);
    const Half g = static_cast<Half>((value >> 8) & 0xFFu);
    const Half b = static_cast<Half>((value >> 16) & 0xFFu);
    return static_cast<Half>((r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10));
}

GPU::Half GPU::rgb24_to_bgr15_dithered(Word value, core::s32 x, core::s32 y) const noexcept
{
    if (!dither_enabled_)
        return rgb24_to_bgr15(value);

    // GPU dither matrix, indexed in absolute framebuffer coordinates.
    static constexpr int matrix[4][4] = {
        {-4, 0, -3, 1}, {2, -2, 3, -1}, {-3, 1, -4, 0}, {3, -1, 2, -2}
    };
    const int d = matrix[static_cast<unsigned>(y) & 3u][static_cast<unsigned>(x) & 3u];
    const auto channel = [d](Word rgb, unsigned shift) {
        return std::clamp(static_cast<int>((rgb >> shift) & 0xFFu) + d, 0, 255);
    };
    const int r = channel(value, 0), g = channel(value, 8), b = channel(value, 16);
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
    plot(x, y, rgb24_to_bgr15_dithered(color, x, y));
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
    if (packet_kind_ == PacketKind::CpuToVram && packet_expected_ == 0)
    {
        execute_gp0_cpu_to_vram_word(value);
        return;
    }
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
    refresh_ready_status();
    status_ &= ~(1u << 26); // command word has been accepted; parameters follow.

    if (opcode == 0x80u)
    {
        packet_kind_ = PacketKind::VramToVram;
        packet_expected_ = 4; // command, source, destination, dimensions
        refresh_ready_status();
        return;
    }
    if (opcode == 0xA0u)
    {
        packet_kind_ = PacketKind::CpuToVram;
        packet_expected_ = 3; // command, destination, dimensions
        vram_to_cpu_ = false;
        refresh_ready_status();
        return;
    }
    if (opcode == 0xC0u)
    {
        packet_kind_ = PacketKind::VramToCpu;
        packet_expected_ = 3;
        vram_to_cpu_ = false;
        refresh_ready_status();
        return;
    }

    if (opcode == 0x02u)
    {
        packet_kind_ = PacketKind::QuickFill;
        packet_expected_ = 3;
        refresh_ready_status();
        return;
    }

    if (opcode == 0x1Fu)
    {
        gpu_irq_ = true;
        refresh_ready_status();
        reset_packet();
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
        status_ &= ~(1u << 28);
        refresh_ready_status();
        return;
    }

    if (group == 2u)
    {
        const bool polyline = (command & (1u << 27)) != 0;
        const bool gouraud = (command & (1u << 28)) != 0;
        packet_kind_ = PacketKind::Line;
        packet_polyline_ = polyline;
        packet_expected_ = polyline ? gp0_packet_.size() : (gouraud ? 4 : 3);
        status_ &= ~(1u << 28);
        refresh_ready_status();
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
        refresh_ready_status();
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
    if (packet_polyline_ && (value & 0xF000F000u) == 0x50005000u)
    {
        if (packet_size_ >= 3)
        {
            execute_gp0_line();
            gpu_busy_cycles_ = estimate_packet_cycles();
        }
        reset_packet();
        return;
    }

    if (packet_size_ >= gp0_packet_.size())
    {
        reset_packet();
        return;
    }
    gp0_packet_[packet_size_++] = value;
    if (!packet_polyline_ && packet_size_ >= packet_expected_)
    {
        execute_packet();
        if (packet_kind_ != PacketKind::CpuToVram &&
            packet_kind_ != PacketKind::VramToCpu)
            reset_packet();
    }
}

core::u32 GPU::estimate_packet_cycles() const noexcept
{
    // The public PSX references document FIFO/status behaviour, but do not
    // provide a portable per-primitive execution-cycle table.  Stage 7 uses
    // an explicit deterministic renderer-cost model instead of pretending
    // that an invented number is measured hardware timing.
    const core::u32 words = static_cast<core::u32>(packet_expected_ ? packet_expected_ : 1);
    core::u64 pixels = 0;
    switch (packet_kind_)
    {
    case PacketKind::VramToVram:
    {
        const core::u32 size = gp0_packet_[3];
        const core::u32 width = (((size & 0xFFFFu) - 1u) & 0x3FFu) + 1u;
        const core::u32 height = ((((size >> 16) & 0xFFFFu) - 1u) & 0x1FFu) + 1u;
        pixels = static_cast<core::u64>(width) * height;
        break;
    }
    case PacketKind::QuickFill:
    {
        const core::u32 size = gp0_packet_[2];
        pixels = static_cast<core::u64>(size & 0x3FFu) * 16u * ((size >> 16) & 0x1FFu);
        break;
    }
    case PacketKind::Rectangle:
    {
        const Word c = gp0_packet_[0];
        const core::u32 code = (c >> 27) & 3u;
        if (code == 1) pixels = 1; else if (code == 2) pixels = 64; else if (code == 3) pixels = 256;
        else { const Word size = (c & (1u << 26)) ? gp0_packet_[3] : gp0_packet_[2]; pixels = static_cast<core::u64>(size & 0x3FFu) * ((size >> 16) & 0x1FFu); }
        break;
    }
    case PacketKind::Line:
    {
        const Word c = gp0_packet_[0];
        const bool gouraud = (c & (1u << 28)) != 0;
        const Vertex a = unpack_vertex(gp0_packet_[1]);
        const Vertex b = unpack_vertex(gp0_packet_[gouraud ? 3 : 2]);
        pixels = static_cast<core::u64>(std::max(std::abs(b.x - a.x), std::abs(b.y - a.y))) + 1u;
        break;
    }
    case PacketKind::Polygon:
    {
        const Word c = gp0_packet_[0];
        const bool quad = (c & (1u << 27)) != 0;
        const std::size_t n = quad ? 4 : 3;
        std::size_t i = 1; std::array<Vertex,4> v{};
        const bool gouraud=(c&(1u<<28))!=0, textured=(c&(1u<<26))!=0;
        for(std::size_t k=0;k<n;++k){ if(gouraud&&k>0) ++i; v[k]=unpack_vertex(gp0_packet_[i++]); if(textured) ++i; }
        auto area=[](Vertex a,Vertex b,Vertex c)->core::u64 { const core::s64 a2=std::llabs(static_cast<core::s64>(b.x-a.x)*(c.y-a.y)-static_cast<core::s64>(b.y-a.y)*(c.x-a.x)); return static_cast<core::u64>(a2)/2u; };
        pixels=area(v[0],v[1],v[2]); if(quad) pixels+=area(v[1],v[2],v[3]);
        break;
    }
    default: break;
    }
    return std::max<core::u32>(1u, words + static_cast<core::u32>((pixels + 7u) / 8u));
}

void GPU::execute_packet()
{
    switch (packet_kind_)
    {
    case PacketKind::QuickFill: execute_gp0_quick_fill(); break;
    case PacketKind::VramToVram:
        execute_gp0_vram_to_vram();
        break;
    case PacketKind::VramToCpu:
        begin_vram_to_cpu(packet_command_);
        return;
    case PacketKind::CpuToVram:
        transfer_x_ = static_cast<core::u16>(gp0_packet_[1] & 0x3FFu);
        transfer_y_ = static_cast<core::u16>((gp0_packet_[1] >> 16) & 0x1FFu);
        {
            const core::u32 raw_w = gp0_packet_[2] & 0xFFFFu;
            const core::u32 raw_h = (gp0_packet_[2] >> 16) & 0xFFFFu;
            transfer_width_ = static_cast<core::u16>(((raw_w - 1u) & 0x3FFu) + 1u);
            transfer_height_ = static_cast<core::u16>(((raw_h - 1u) & 0x1FFu) + 1u);
        }
        transfer_pixel_index_ = 0;
        transfer_words_remaining_ = (static_cast<core::u32>(transfer_width_) *
                                      transfer_height_ + 1u) / 2u;
        packet_size_ = 0;
        packet_expected_ = 0;
        refresh_ready_status();
        return;
    case PacketKind::Polygon:  execute_gp0_polygon(); break;
    case PacketKind::Line:     execute_gp0_line(); break;
    case PacketKind::Rectangle: execute_gp0_rectangle(); break;
    case PacketKind::None: break;
    }
    gpu_busy_cycles_ = estimate_packet_cycles();
    refresh_ready_status();
}

void GPU::tick() noexcept
{
    if (gpu_busy_cycles_ != 0) --gpu_busy_cycles_;
    if (gpu_busy_cycles_ == 0) refresh_ready_status();
}

void GPU::tick(core::u32 cycles) noexcept
{
    while (cycles--) tick();
}

void GPU::execute_gp0_cpu_to_vram_word(Word value) noexcept
{
    const core::u32 total = static_cast<core::u32>(transfer_width_) * transfer_height_;
    for (unsigned half = 0; half < 2; ++half)
    {
        const core::u32 pixel = transfer_pixel_index_++;
        if (pixel >= total) break;
        const core::u32 x = (static_cast<core::u32>(transfer_x_) + pixel % transfer_width_) % VRAM_WIDTH;
        const core::u32 y = (static_cast<core::u32>(transfer_y_) + pixel / transfer_width_) % VRAM_HEIGHT;
        Half& dst = vram_[y * VRAM_WIDTH + x];
        if (mask_check_ && (dst & 0x8000u) != 0)
            continue;
        Half data = static_cast<Half>(value >> (half * 16u));
        if (mask_set_)
            data = static_cast<Half>(data | 0x8000u);
        dst = data;
    }
    if (transfer_words_remaining_ != 0) --transfer_words_remaining_;
    if (transfer_words_remaining_ == 0) reset_packet();
}

void GPU::begin_vram_to_cpu(Word command) noexcept
{
    (void)command;
    transfer_x_ = static_cast<core::u16>(gp0_packet_[1] & 0x3FFu);
    transfer_y_ = static_cast<core::u16>((gp0_packet_[1] >> 16) & 0x1FFu);
    const core::u32 raw_w = gp0_packet_[2] & 0xFFFFu;
    const core::u32 raw_h = (gp0_packet_[2] >> 16) & 0xFFFFu;
    transfer_width_ = static_cast<core::u16>(((raw_w - 1u) & 0x3FFu) + 1u);
    transfer_height_ = static_cast<core::u16>(((raw_h - 1u) & 0x1FFu) + 1u);
    read_transfer_pixel_index_ = 0;
    vram_to_cpu_ = true;
    packet_size_ = packet_expected_ = 0;
    packet_kind_ = PacketKind::None;
    packet_command_ = 0;
    packet_polyline_ = false;
    gpu_busy_cycles_ = 0;
    refresh_ready_status();
}

void GPU::execute_gp0_environment(Word command)
{
    const Word opcode = command >> 24;
    switch (opcode)
    {
    case 0xE1u:
        dither_enabled_ = (command & (1u << 9)) != 0;
        draw_to_display_area_ = (command & (1u << 10)) != 0;
        tpage_ = static_cast<core::u16>(command & 0x3FFFu);
        status_ = (status_ & ~0x1FFFu) | (command & 0x1FFFu);
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
        draw_offset_x_ = sign_extend_11(command);
        draw_offset_y_ = sign_extend_11(command >> 11);
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
        status_ = (status_ & ~((1u << 11) | (1u << 12))) |
                  (mask_set_ ? (1u << 11) : 0u) |
                  (mask_check_ ? (1u << 12) : 0u);
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

    const core::s32 x0 = static_cast<core::s32>(origin & 0x3F0u);
    const core::s32 y0 = static_cast<core::s32>((origin >> 16) & 0x1FFu);
    const core::s32 width = static_cast<core::s32>(((size & 0x3FFu) + 0xFu) & ~0xFu);
    const core::s32 height = static_cast<core::s32>((size >> 16) & 0x1FFu);
    if (width == 0 || height == 0)
        return;

    const Half pixel = rgb24_to_bgr15(color);
    for (core::s32 y = 0; y < height; ++y)
        for (core::s32 x = 0; x < width; ++x)
        {
            const core::u32 dst_x = (static_cast<core::u32>(x0 + x)) % VRAM_WIDTH;
            const core::u32 dst_y = (static_cast<core::u32>(y0 + y)) % VRAM_HEIGHT;
            // GP0(02h) ignores mask-bit state and drawing-area clipping.
            vram_[dst_y * VRAM_WIDTH + dst_x] = pixel;
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
    const core::u32 page_y = (((static_cast<core::u32>(info.tpage) >> 4) & 1u) * 256u) +
                              (((static_cast<core::u32>(info.tpage) >> 11) & 1u) * 512u);
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
    const core::u32 page_y = (((static_cast<core::u32>(info.tpage) >> 4) & 1u) * 256u) +
                              (((static_cast<core::u32>(info.tpage) >> 11) & 1u) * 512u);
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
            TexCoord tc{static_cast<core::u8>(uv.u + ((tpage_ & (1u << 12)) ? -x : x)),
                          static_cast<core::u8>(uv.v + ((tpage_ & (1u << 13)) ? -y : y))};
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
                plot_semi(x, y, rgb24_to_bgr15_dithered(ca, x, y), semi);
        }
    }
}

void GPU::raster_line(Vertex a, Vertex b, Word ca, Word cb, bool gouraud, bool semi)
{
    const core::s32 dx_abs = std::abs(b.x - a.x);
    const core::s32 dy_abs = std::abs(b.y - a.y);
    if (dx_abs > 1023 || dy_abs > 511)
        return;

    const core::s32 dx = dx_abs;
    const core::s32 sx = a.x < b.x ? 1 : -1;
    const core::s32 dy = -dy_abs;
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
            plot_semi(x, y, rgb24_to_bgr15_dithered(color, x, y), semi);
        }
        else
            plot_semi(x, y, rgb24_to_bgr15_dithered(ca, x, y), semi);
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

void GPU::execute_gp0_vram_to_vram()
{
    transfer_source_x_ = static_cast<core::u16>(gp0_packet_[1] & 0x3FFu);
    transfer_source_y_ = static_cast<core::u16>((gp0_packet_[1] >> 16) & 0x1FFu);
    transfer_destination_x_ = static_cast<core::u16>(gp0_packet_[2] & 0x3FFu);
    transfer_destination_y_ = static_cast<core::u16>((gp0_packet_[2] >> 16) & 0x1FFu);

    const core::u32 raw_w = gp0_packet_[3] & 0xFFFFu;
    const core::u32 raw_h = (gp0_packet_[3] >> 16) & 0xFFFFu;
    const core::u32 width = ((raw_w - 1u) & 0x3FFu) + 1u;
    const core::u32 height = ((raw_h - 1u) & 0x1FFu) + 1u;

    // Snapshot the source because overlapping copies are defined as a VRAM
    // transfer rather than a C++ memmove operation; the hardware reads the
    // source image as it proceeds through the transfer.
    std::vector<Half> source(static_cast<std::size_t>(width) * height);
    for (core::u32 y = 0; y < height; ++y)
        for (core::u32 x = 0; x < width; ++x)
        {
            const core::u32 sx = (transfer_source_x_ + x) % VRAM_WIDTH;
            const core::u32 sy = (transfer_source_y_ + y) % VRAM_HEIGHT;
            source[static_cast<std::size_t>(y) * width + x] = vram_[sy * VRAM_WIDTH + sx];
        }

    for (core::u32 y = 0; y < height; ++y)
        for (core::u32 x = 0; x < width; ++x)
        {
            const core::u32 dx = (transfer_destination_x_ + x) % VRAM_WIDTH;
            const core::u32 dy = (transfer_destination_y_ + y) % VRAM_HEIGHT;
            Half& dst = vram_[dy * VRAM_WIDTH + dx];
            if (mask_check_ && (dst & 0x8000u) != 0)
                continue;
            Half data = source[static_cast<std::size_t>(y) * width + x];
            if (mask_set_)
                data = static_cast<Half>(data | 0x8000u);
            dst = data;
        }
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

    auto valid_edge = [](Vertex a, Vertex b) {
        return std::abs(b.x - a.x) <= 1023 && std::abs(b.y - a.y) <= 511;
    };
    if (!valid_edge(v[0], v[1]) || !valid_edge(v[1], v[2]) || !valid_edge(v[2], v[0]) ||
        (quad && (!valid_edge(v[2], v[3]) || !valid_edge(v[3], v[0]) || !valid_edge(v[1], v[3]))))
        return;

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
    const bool semi = (command & (1u << 25)) != 0;
    const Word c0 = rgb_components(command);

    if (packet_polyline_)
    {
        std::size_t index = 1;
        Vertex previous{};
        Word previous_color = c0;
        bool have_previous = false;
        while (index < packet_size_)
        {
            Word color = previous_color;
            if (have_previous && gouraud)
            {
                if (index >= packet_size_)
                    break;
                color = rgb_components(gp0_packet_[index++]);
            }
            if (index >= packet_size_)
                break;
            const Vertex current = unpack_vertex(gp0_packet_[index++]);
            if (have_previous)
                raster_line(previous, current, previous_color, color, gouraud, semi);
            previous = current;
            previous_color = color;
            have_previous = true;
        }
        return;
    }

    Vertex a = unpack_vertex(gp0_packet_[1]);
    Vertex b = unpack_vertex(gp0_packet_[2]);
    Word c1 = c0;
    if (gouraud)
    {
        c1 = rgb_components(gp0_packet_[2]);
        b = unpack_vertex(gp0_packet_[3]);
    }
    raster_line(a, b, c0, c1, gouraud, semi);
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
    dma_direction_ = 0;
    gpu_irq_ = false;
    draw_left_ = 0;
    draw_top_ = 0;
    draw_right_ = 1023;
    draw_bottom_ = 511;
    draw_offset_x_ = 0;
    draw_offset_y_ = 0;
    mask_set_ = false;
    mask_check_ = false;
    dither_enabled_ = false;
    draw_to_display_area_ = false;
    gpu_busy_cycles_ = 0;
    transfer_x_ = transfer_y_ = transfer_width_ = transfer_height_ = 0;
    transfer_source_x_ = transfer_source_y_ = 0;
    transfer_destination_x_ = transfer_destination_y_ = 0;
    transfer_words_remaining_ = transfer_pixel_index_ = 0;
    read_transfer_pixel_index_ = 0;
    vram_to_cpu_ = false;
    gp0_read_latch_ = 0;
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

    // GP1(10h..1Fh) are mirrors of the internal-register read command.
    if (opcode >= 0x10u && opcode <= 0x1Fu)
    {
        switch (value & 0x0Fu)
        {
        case 0x02u:
            gp0_read_latch_ = static_cast<Word>(texture_window_mask_x_) |
                              (static_cast<Word>(texture_window_mask_y_) << 5) |
                              (static_cast<Word>(texture_window_offset_x_) << 10) |
                              (static_cast<Word>(texture_window_offset_y_) << 15);
            break;
        case 0x03u:
            gp0_read_latch_ = static_cast<Word>(draw_left_) | (static_cast<Word>(draw_top_) << 10);
            break;
        case 0x04u:
            gp0_read_latch_ = static_cast<Word>(draw_right_) | (static_cast<Word>(draw_bottom_) << 10);
            break;
        case 0x05u:
            gp0_read_latch_ = (static_cast<Word>(draw_offset_x_) & 0x7FFu) |
                              ((static_cast<Word>(draw_offset_y_) & 0x7FFu) << 11);
            break;
        case 0x07u:
            gp0_read_latch_ = 2u;
            break;
        default:
            break;
        }
        return;
    }

    switch (opcode)
    {
    case 0x00u: gp1_reset(); break;
    case 0x01u:
        reset_packet();
        vram_to_cpu_ = false;
        read_transfer_pixel_index_ = 0;
        break;
    case 0x02u:
        gpu_irq_ = false;
        refresh_ready_status();
        break;
    case 0x03u:
        display_enabled_ = (value & 1u) == 0;
        if (display_enabled_) status_ |= 1u << 23; else status_ &= ~(1u << 23);
        break;
    case 0x04u:
        dma_direction_ = static_cast<core::u8>(value & 3u);
        refresh_ready_status();
        break;
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
        display_mode_ = static_cast<core::u8>(value & 0xFFu);
        status_ = (status_ & ~((3u << 17) | (1u << 19) | (1u << 20) | (1u << 21) | (1u << 22))) |
                  ((static_cast<Word>(display_mode_) & 3u) << 17) |
                  (((static_cast<Word>(display_mode_) >> 2) & 1u) << 19) |
                  (((static_cast<Word>(display_mode_) >> 3) & 1u) << 20) |
                  (((static_cast<Word>(display_mode_) >> 4) & 1u) << 21) |
                  (((static_cast<Word>(display_mode_) >> 5) & 1u) << 22);
        if ((display_mode_ & (1u << 5)) == 0) status_ |= 1u << 13;
        else status_ &= ~(1u << 13);
        break;
    case 0x09u:
        // Retail model: 1 MB VRAM. GP1(09h) is a v2/2 MB configuration control.
        break;
    default:
        break;
    }
}

GPU::Word GPU::read_gp0() const noexcept
{
    if (!vram_to_cpu_) return gp0_read_latch_;
    const core::u32 total = static_cast<core::u32>(transfer_width_) * transfer_height_;
    Word result = 0;
    for (unsigned half = 0; half < 2; ++half)
    {
        const core::u32 pixel = read_transfer_pixel_index_++;
        if (pixel >= total) break;
        const core::u32 x = (static_cast<core::u32>(transfer_x_) + pixel % transfer_width_) % VRAM_WIDTH;
        const core::u32 y = (static_cast<core::u32>(transfer_y_) + pixel / transfer_width_) % VRAM_HEIGHT;
        result |= static_cast<Word>(vram_[y * VRAM_WIDTH + x]) << (half * 16u);
    }
    if (read_transfer_pixel_index_ >= total) vram_to_cpu_ = false;
    refresh_ready_status();
    return result;
}

GPU::Word GPU::read_gp1() const noexcept
{
    return status_;
}

} // namespace imatfe::gpu
