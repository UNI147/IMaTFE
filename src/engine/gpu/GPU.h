#pragma once

#include "engine/core/Types.h"

#include <array>
#include <cstddef>
#include <span>
#include <vector>

namespace imatfe::gpu
{

class GPU final
{
public:
    using Word = core::u32;
    using Half = core::u16;

    static constexpr std::size_t VRAM_WIDTH = 1024;
    static constexpr std::size_t VRAM_HEIGHT = 512;

    struct Vertex
    {
        core::s32 x = 0;
        core::s32 y = 0;
    };

    GPU();

    void reset() noexcept;

    void write_gp0(Word value);
    void write_gp1(Word value);
    Word read_gp0() const noexcept;
    Word read_gp1() const noexcept;

    Half vram(std::size_t x, std::size_t y) const noexcept;
    void set_vram(std::size_t x, std::size_t y, Half value) noexcept;
    void clear_vram(Half color = 0) noexcept;
    std::span<const Half> vram() const noexcept { return vram_; }
    std::span<Half> vram() noexcept { return vram_; }

    bool display_enabled() const noexcept { return display_enabled_; }
    std::size_t draw_area_left() const noexcept { return draw_left_; }
    std::size_t draw_area_top() const noexcept { return draw_top_; }
    std::size_t draw_area_right() const noexcept { return draw_right_; }
    std::size_t draw_area_bottom() const noexcept { return draw_bottom_; }

private:
    enum class PacketKind
    {
        None,
        QuickFill,
        Polygon,
        Line,
        Rectangle,
    };

    std::vector<Half> vram_;
    std::array<Word, 16> gp0_packet_{};
    std::size_t packet_size_ = 0;
    std::size_t packet_expected_ = 0;
    PacketKind packet_kind_ = PacketKind::None;
    Word packet_command_ = 0;

    Word status_ = 0x14802000u;
    bool display_enabled_ = true;
    core::u16 display_x_ = 0;
    core::u16 display_y_ = 0;
    core::u16 display_x1_ = 0x200;
    core::u16 display_x2_ = 0xC00;
    core::u16 display_y1_ = 0x10;
    core::u16 display_y2_ = 0x100;
    core::u8 display_mode_ = 0;

    std::size_t draw_left_ = 0;
    std::size_t draw_top_ = 0;
    std::size_t draw_right_ = 1023;
    std::size_t draw_bottom_ = 511;
    core::s32 draw_offset_x_ = 0;
    core::s32 draw_offset_y_ = 0;
    bool mask_set_ = false;
    bool mask_check_ = false;
    bool dither_enabled_ = false;

    // GPU draw/texture state.  The PSX keeps texture pages and the texture
    // window in GPU state; textured polygons carry their own TPage/CLUT
    // attributes in the UV words.
    core::u16 tpage_ = 0;
    core::u8 texture_window_mask_x_ = 0;
    core::u8 texture_window_mask_y_ = 0;
    core::u8 texture_window_offset_x_ = 0;
    core::u8 texture_window_offset_y_ = 0;

    void reset_packet() noexcept;
    void begin_gp0(Word command);
    void consume_packet_word(Word value);
    void execute_packet();

    void execute_gp0_environment(Word command);
    void execute_gp0_quick_fill();
    void execute_gp0_polygon();
    void execute_gp0_line();
    void execute_gp0_rectangle();

    void gp1_reset() noexcept;
    void gp1_command(Word value) noexcept;

    static core::s16 signed16(Word value) noexcept;
    static Vertex unpack_vertex(Word value) noexcept;
    static Half rgb24_to_bgr15(Word value) noexcept;
    Half rgb24_to_bgr15_dithered(Word value, core::s32 x, core::s32 y) const noexcept;
    static Word rgb_components(Word value) noexcept;
    static Half interpolate_rgb(Word a, Word b, Word c, double wa, double wb, double wc) noexcept;

    struct TexCoord
    {
        core::u8 u = 0;
        core::u8 v = 0;
    };

    struct TextureInfo
    {
        core::u16 tpage = 0;
        core::u16 clut = 0;
        bool raw = false;
    };

    static TextureInfo decode_texture_info(core::u32 uv0, core::u32 uv1, bool raw) noexcept;
    TexCoord apply_texture_window(TexCoord uv) const noexcept;
    core::u8 fetch_texture_index(TexCoord uv, const TextureInfo& info) const noexcept;
    Half fetch_clut_color(core::u8 index, const TextureInfo& info) const noexcept;
    Half sample_texture(TexCoord uv, const TextureInfo& info) const noexcept;
    Half modulate_texture(Half texel, Word color) const noexcept;
    Half blend_semi_transparent(Half foreground, Half background) const noexcept;
    void plot_semi(core::s32 x, core::s32 y, Half color, bool enabled, bool stp = true) noexcept;

    void plot(core::s32 x, core::s32 y, Half color) noexcept;
    void plot_rgb(core::s32 x, core::s32 y, Word color) noexcept;
    void raster_triangle(Vertex a, Vertex b, Vertex c, Word ca, Word cb, Word cc, bool gouraud, bool semi);
    void raster_textured_triangle(Vertex a, Vertex b, Vertex c,
                                  TexCoord ua, TexCoord ub, TexCoord uc,
                                  Word ca, Word cb, Word cc,
                                  bool gouraud, bool semi, const TextureInfo& texture);
    void raster_line(Vertex a, Vertex b, Word ca, Word cb, bool gouraud);
    void raster_rectangle(Vertex origin, core::s32 width, core::s32 height, Word color, bool semi);
    void raster_textured_rectangle(Vertex origin, TexCoord uv, core::s32 width, core::s32 height,
                                   const TextureInfo& texture, bool semi);
};

} // namespace imatfe::gpu
