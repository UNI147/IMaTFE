#include "GTE.h"

#include <algorithm>
#include <limits>

namespace imatfe::gte
{
namespace
{
constexpr core::u32 FLAG_MAC1_POS = 1u << 30;
constexpr core::u32 FLAG_MAC2_POS = 1u << 29;
constexpr core::u32 FLAG_MAC3_POS = 1u << 28;
constexpr core::u32 FLAG_MAC1_NEG = 1u << 27;
constexpr core::u32 FLAG_MAC2_NEG = 1u << 26;
constexpr core::u32 FLAG_MAC3_NEG = 1u << 25;
constexpr core::u32 FLAG_IR1_SAT  = 1u << 24;
constexpr core::u32 FLAG_IR2_SAT  = 1u << 23;
constexpr core::u32 FLAG_IR3_SAT  = 1u << 22;
constexpr core::u32 FLAG_R_SAT    = 1u << 21;
constexpr core::u32 FLAG_G_SAT    = 1u << 20;
constexpr core::u32 FLAG_B_SAT    = 1u << 19;
constexpr core::u32 FLAG_SZ_SAT   = 1u << 18;
constexpr core::u32 FLAG_DIV_OVF  = 1u << 17;
constexpr core::u32 FLAG_MAC0_POS = 1u << 16;
constexpr core::u32 FLAG_MAC0_NEG = 1u << 15;
constexpr core::u32 FLAG_SX_SAT   = 1u << 14;
constexpr core::u32 FLAG_SY_SAT   = 1u << 13;
constexpr core::u32 FLAG_IR0_SAT  = 1u << 12;
constexpr core::u32 FLAG_WRITABLE = 0x7F87E000u;

constexpr core::s64 MAC44_MIN = -(core::s64(1) << 43);
constexpr core::s64 MAC44_MAX =  (core::s64(1) << 43) - 1;

constexpr core::s64 clamp_s64(core::s64 v, core::s64 lo, core::s64 hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

constexpr core::u32 command_opcode(core::u32 instruction) noexcept
{
    return instruction & 0x3Fu;
}
}

GTE::GTE() { reset(); }

void GTE::reset() noexcept
{
    data_.fill(0);
    control_.fill(0);
    busy_cycles_ = 0;
}

core::s16 GTE::lo16(Word value) noexcept { return static_cast<core::s16>(value & 0xFFFFu); }
core::s16 GTE::hi16(Word value) noexcept { return static_cast<core::s16>((value >> 16) & 0xFFFFu); }
GTE::Word GTE::pack16(core::s16 lo, core::s16 hi) noexcept
{
    return static_cast<Word>(static_cast<core::u16>(lo)) |
           (static_cast<Word>(static_cast<core::u16>(hi)) << 16);
}
core::s32 GTE::sext16(core::u16 value) noexcept { return static_cast<core::s32>(static_cast<core::s16>(value)); }

core::s64 GTE::sar(core::s64 value, unsigned bits) noexcept
{
    if (bits == 0) return value;
    if (value >= 0) return value >> bits;
    const core::s64 magnitude = -value;
    return -((magnitude + ((core::s64(1) << bits) - 1)) >> bits);
}

core::s32 GTE::data_s16(core::u8 index) const noexcept
{
    const Word v = data_[index & 31u];
    return sext16(static_cast<core::u16>(v));
}

core::u16 GTE::data_u16(core::u8 index) const noexcept
{
    return static_cast<core::u16>(data_[index & 31u]);
}

core::s32 GTE::control_s16(core::u8 index) const noexcept
{
    return sext16(static_cast<core::u16>(control_[index & 31u]));
}

void GTE::clear_flag() noexcept { control_[31] = 0; }

void GTE::set_flag(core::u32 bits) noexcept
{
    control_[31] |= bits & FLAG_WRITABLE;
    if (control_[31] & FLAG_WRITABLE)
        control_[31] |= 0x80000000u;
    else
        control_[31] &= ~0x80000000u;
}

void GTE::write_mac(int index, core::s64 value) noexcept
{
    const core::u32 pos = index == 1 ? FLAG_MAC1_POS : index == 2 ? FLAG_MAC2_POS : FLAG_MAC3_POS;
    const core::u32 neg = index == 1 ? FLAG_MAC1_NEG : index == 2 ? FLAG_MAC2_NEG : FLAG_MAC3_NEG;
    if (value > MAC44_MAX) set_flag(pos);
    if (value < MAC44_MIN) set_flag(neg);
    data_[24 + index] = static_cast<Word>(static_cast<core::s32>(value));
}

void GTE::write_mac0(core::s64 value) noexcept
{
    if (value > std::numeric_limits<core::s32>::max()) set_flag(FLAG_MAC0_POS);
    if (value < std::numeric_limits<core::s32>::min()) set_flag(FLAG_MAC0_NEG);
    data_[24] = static_cast<Word>(static_cast<core::s32>(value));
}

core::s32 GTE::saturate_ir(core::s64 value, bool lm, int index) noexcept
{
    const core::s64 lo = lm ? 0 : -32768;
    const core::s64 hi = 32767;
    if (value < lo || value > hi)
        set_flag(index == 1 ? FLAG_IR1_SAT : index == 2 ? FLAG_IR2_SAT : FLAG_IR3_SAT);
    return static_cast<core::s32>(clamp_s64(value, lo, hi));
}

core::u16 GTE::saturate_sz(core::s64 value) noexcept
{
    if (value < 0 || value > 0xFFFF) set_flag(FLAG_SZ_SAT);
    return static_cast<core::u16>(clamp_s64(value, 0, 0xFFFF));
}

core::u16 GTE::saturate_otz(core::s64 value) noexcept
{
    if (value < 0 || value > 0xFFFF) set_flag(FLAG_SZ_SAT);
    return static_cast<core::u16>(clamp_s64(value, 0, 0xFFFF));
}

core::s16 GTE::saturate_screen(core::s64 value, bool y) noexcept
{
    constexpr core::s64 lo = -0x400;
    constexpr core::s64 hi = 0x3FF;
    if (value < lo || value > hi) set_flag(y ? FLAG_SY_SAT : FLAG_SX_SAT);
    return static_cast<core::s16>(clamp_s64(value, lo, hi));
}

core::u16 GTE::irgb_component(core::s32 value) const noexcept
{
    return static_cast<core::u16>(clamp_s64(sar(value, 7), 0, 0x1F));
}

core::u32 GTE::read_lzcr() const noexcept
{
    const core::u32 x = data_[30];
    const bool negative = (x & 0x80000000u) != 0;
    const core::u32 probe = negative ? ~x : x;
    core::u32 count = 0;
    while (count < 32 && (probe & (0x80000000u >> count)) == 0)
        ++count;
    return count;
}

GTE::Word GTE::read_data(core::u8 index) const noexcept
{
    index &= 31u;
    switch (index)
    {
    case 7: return data_[7];
    case 15: return data_[14];
    case 28:
    {
        const core::u16 r = irgb_component(data_s16(9));
        const core::u16 g = irgb_component(data_s16(10));
        const core::u16 b = irgb_component(data_s16(11));
        return r | (static_cast<Word>(g) << 5) | (static_cast<Word>(b) << 10);
    }
    case 29:
    {
        const core::u16 r = irgb_component(data_s16(9));
        const core::u16 g = irgb_component(data_s16(10));
        const core::u16 b = irgb_component(data_s16(11));
        return r | (static_cast<Word>(g) << 5) | (static_cast<Word>(b) << 10);
    }
    case 31: return read_lzcr();
    default: return data_[index];
    }
}

void GTE::write_data(core::u8 index, Word value) noexcept
{
    index &= 31u;
    switch (index)
    {
    case 7: case 23: case 29: case 31:
        return;
    case 15:
        push_sxy(lo16(value), hi16(value));
        return;
    case 28:
        data_[28] = value & 0x7FFFu;
        data_[9] = static_cast<Word>((value & 0x1Fu) << 7);
        data_[10] = static_cast<Word>(((value >> 5) & 0x1Fu) << 7);
        data_[11] = static_cast<Word>(((value >> 10) & 0x1Fu) << 7);
        return;
    case 30:
        data_[30] = value;
        data_[31] = read_lzcr();
        return;
    default:
        data_[index] = value;
        return;
    }
}

GTE::Word GTE::read_control(core::u8 index) const noexcept
{
    index &= 31u;
    if (index == 31) return control_[31];
    // H is unsigned internally but CFC2 sign-extends its low halfword on real hardware.
    if (index == 26) return static_cast<Word>(static_cast<core::s32>(static_cast<core::s16>(control_[26] & 0xFFFFu)));
    if (index == 4 || index == 12 || index == 20 || index == 27 || index == 29 || index == 30)
        return static_cast<Word>(static_cast<core::s32>(control_s16(index)));
    return control_[index];
}

void GTE::write_control(core::u8 index, Word value) noexcept
{
    index &= 31u;
    if (index == 31)
    {
        control_[31] = value & FLAG_WRITABLE;
        if (control_[31] & FLAG_WRITABLE) control_[31] |= 0x80000000u;
        return;
    }
    control_[index] = value;
}

core::s64 GTE::matrix_element(core::u8 mx, int row, int col) const noexcept
{
    const int base = mx == 0 ? 0 : mx == 1 ? 8 : 16;
    const int element = row * 3 + col;
    const int packed = base + element / 2;
    return (element & 1) ? hi16(control_[packed]) : lo16(control_[packed]);
}

core::s64 GTE::vector_element(core::u8 v, int index) const noexcept
{
    if (v == 3) return data_s16(static_cast<core::u8>(9 + index));
    const int base = v == 0 ? 0 : v == 1 ? 2 : 4;
    return index == 0 ? lo16(data_[base]) : index == 1 ? hi16(data_[base]) : data_s16(static_cast<core::u8>(base + 1));
}

core::s64 GTE::constant_element(core::u8 cv, int index) const noexcept
{
    switch (cv & 3u)
    {
    case 0: return control_s16(static_cast<core::u8>(5 + index));
    case 1: return control_s16(static_cast<core::u8>(13 + index));
    case 2: return control_s16(static_cast<core::u8>(21 + index));
    default: return data_s16(static_cast<core::u8>(9 + index));
    }
}

void GTE::push_sxy(core::s16 x, core::s16 y) noexcept
{
    data_[12] = data_[13];
    data_[13] = data_[14];
    data_[14] = pack16(x, y);
    data_[15] = data_[14];
}

void GTE::push_sz(core::u16 z) noexcept
{
    data_[16] = data_[17];
    data_[17] = data_[18];
    data_[18] = data_[19];
    data_[19] = z;
}

void GTE::push_rgb(core::s64 mac1, core::s64 mac2, core::s64 mac3) noexcept
{
    data_[20] = data_[21];
    data_[21] = data_[22];

    auto channel = [&](core::s64 mac, core::u32 bit) -> core::u32 {
        const core::s64 v = sar(mac, 4);
        if (v < 0 || v > 255) set_flag(bit);
        return static_cast<core::u32>(clamp_s64(v, 0, 255));
    };

    const core::u32 r = channel(mac1, FLAG_R_SAT);
    const core::u32 g = channel(mac2, FLAG_G_SAT);
    const core::u32 b = channel(mac3, FLAG_B_SAT);
    const core::u32 code = data_[6] & 0xFF000000u;
    data_[22] = code | r | (g << 8) | (b << 16);
}

void GTE::mvmva(Word instruction) noexcept
{
    const bool sf = ((instruction >> 19) & 1u) != 0;
    const core::u8 mx = static_cast<core::u8>((instruction >> 17) & 3u);
    const core::u8 v  = static_cast<core::u8>((instruction >> 15) & 3u);
    const core::u8 cv = static_cast<core::u8>((instruction >> 13) & 3u);
    const bool lm = ((instruction >> 10) & 1u) != 0;

    clear_flag();
    for (int row = 0; row < 3; ++row)
    {
        core::s64 acc;
        if (cv == 3)
            acc = 0;
        else if (cv == 2)
            acc = matrix_element(mx, row, 1) * vector_element(v, 1) + matrix_element(mx, row, 2) * vector_element(v, 2);
        else
            acc = (constant_element(cv, row) << 12);
        for (int col = 0; col < 3; ++col)
            if (!(cv == 2 && col == 0)) acc += matrix_element(mx, row, col) * vector_element(v, col);
        acc = sar(acc, sf ? 12 : 0);
        write_mac(row + 1, acc);
        data_[9 + row] = static_cast<Word>(saturate_ir(acc, lm, row + 1));
    }
}

void GTE::transform_vertex(int vertex, Word instruction, core::s16& sx, core::s16& sy, core::u16& sz) noexcept
{
    const int vb = vertex * 2;
    const core::s64 vx = lo16(data_[vb]);
    const core::s64 vy = hi16(data_[vb]);
    const core::s64 vz = data_s16(static_cast<core::u8>(vb + 1));
    const bool sf = ((instruction >> 19) & 1u) != 0;
    const bool lm = ((instruction >> 10) & 1u) != 0;
    const unsigned shift = sf ? 12u : 0u;

    const core::s64 mac1 = sar((static_cast<core::s32>(control_[5]) << 12) + matrix_element(0, 0, 0) * vx + matrix_element(0, 0, 1) * vy + matrix_element(0, 0, 2) * vz, shift);
    const core::s64 mac2 = sar((static_cast<core::s32>(control_[6]) << 12) + matrix_element(0, 1, 0) * vx + matrix_element(0, 1, 1) * vy + matrix_element(0, 1, 2) * vz, shift);
    const core::s64 mac3 = sar((static_cast<core::s32>(control_[7]) << 12) + matrix_element(0, 2, 0) * vx + matrix_element(0, 2, 1) * vy + matrix_element(0, 2, 2) * vz, shift);

    write_mac(1, mac1); write_mac(2, mac2); write_mac(3, mac3);
    data_[9]  = static_cast<Word>(saturate_ir(mac1, lm, 1));
    data_[10] = static_cast<Word>(saturate_ir(mac2, lm, 2));
    // RTPS/RTPT FLAG.22 is checked with lm=0, regardless of instruction lm.
    if (mac3 < -32768 || mac3 > 32767) set_flag(FLAG_IR3_SAT);
    data_[11] = static_cast<Word>(saturate_ir(mac3, lm, 3));

    sz = saturate_sz(sar(mac3, sf ? 0u : 12u));
    push_sz(sz);

    const core::s64 h = static_cast<core::u16>(control_[26]);
    const core::s64 z = sz;
    core::s64 quotient;
    if (z == 0)
    {
        quotient = 0x1FFFF;
        set_flag(FLAG_DIV_OVF);
    }
    else
    {
        quotient = ((h << 17) / z + 1) >> 1;
        if (quotient > 0x1FFFF)
        {
            quotient = 0x1FFFF;
            set_flag(FLAG_DIV_OVF);
        }
    }

    const core::s64 ofx = static_cast<core::s32>(control_[24]);
    const core::s64 ofy = static_cast<core::s32>(control_[25]);
    const core::s64 mac0x = ofx + mac1 * quotient;
    const core::s64 mac0y = ofy + mac2 * quotient;
    write_mac0(mac0x);
    sx = saturate_screen(sar(mac0x, 16), false);
    sy = saturate_screen(sar(mac0y, 16), true);
    push_sxy(sx, sy);

    const core::s64 dqa = control_s16(27);
    const core::s64 dqb = static_cast<core::s32>(control_[28]);
    const core::s64 mac0p = dqb + dqa * quotient;
    write_mac0(mac0p);
    const core::s64 ir0 = sar(mac0p, 12);
    if (ir0 < 0 || ir0 > 0x1000) set_flag(FLAG_IR0_SAT);
    data_[8] = static_cast<Word>(clamp_s64(ir0, 0, 0x1000));
}

void GTE::rtps(Word instruction, bool triple) noexcept
{
    clear_flag();
    const int count = triple ? 3 : 1;
    for (int i = 0; i < count; ++i)
    {
        core::s16 sx = 0, sy = 0;
        core::u16 sz = 0;
        transform_vertex(i, instruction, sx, sy, sz);
    }
}

void GTE::nclip() noexcept
{
    clear_flag();
    const core::s64 x0 = lo16(data_[12]), y0 = hi16(data_[12]);
    const core::s64 x1 = lo16(data_[13]), y1 = hi16(data_[13]);
    const core::s64 x2 = lo16(data_[14]), y2 = hi16(data_[14]);
    const core::s64 mac0 = x0*y1 + x1*y2 + x2*y0 - x0*y2 - x1*y0 - x2*y1;
    write_mac0(mac0);
}

void GTE::avsz(bool four) noexcept
{
    clear_flag();
    const core::s64 sum = four ? core::s64(data_u16(16)) + data_u16(17) + data_u16(18) + data_u16(19)
                               : core::s64(data_u16(17)) + data_u16(18) + data_u16(19);
    const core::s64 zsf = control_s16(four ? 30 : 29);
    const core::s64 mac0 = sum * zsf;
    write_mac0(mac0);
    data_[7] = saturate_otz(sar(mac0, 12));
}

void GTE::op(Word instruction) noexcept
{
    clear_flag();
    const bool sf = ((instruction >> 19) & 1u) != 0;
    const bool lm = ((instruction >> 10) & 1u) != 0;
    const core::s64 x = data_s16(9), y = data_s16(10), z = data_s16(11);
    const core::s64 d1 = control_s16(0), d2 = control_s16(4), d3 = control_s16(2);
    const core::s64 m1 = sar(z*d2 - y*d3, sf ? 12 : 0);
    const core::s64 m2 = sar(x*d3 - z*d1, sf ? 12 : 0);
    const core::s64 m3 = sar(y*d1 - x*d2, sf ? 12 : 0);
    write_mac(1,m1); write_mac(2,m2); write_mac(3,m3);
    data_[9] = static_cast<Word>(saturate_ir(m1,lm,1));
    data_[10] = static_cast<Word>(saturate_ir(m2,lm,2));
    data_[11] = static_cast<Word>(saturate_ir(m3,lm,3));
}

void GTE::sqr(Word instruction) noexcept
{
    clear_flag();
    const bool sf = ((instruction >> 19) & 1u) != 0;
    const bool lm = ((instruction >> 10) & 1u) != 0;
    for (int i=0;i<3;++i)
    {
        const core::s64 v = data_s16(static_cast<core::u8>(9+i));
        const core::s64 m = sar(v*v, sf ? 12 : 0);
        write_mac(i+1,m);
        data_[9+i] = static_cast<Word>(saturate_ir(m,lm,i+1));
    }
}

void GTE::gpf(Word instruction, bool linear) noexcept
{
    clear_flag();
    const bool sf = ((instruction >> 19) & 1u) != 0;
    const bool lm = ((instruction >> 10) & 1u) != 0;
    const core::s64 ir0 = data_s16(8);
    for (int i=0;i<3;++i)
    {
        const core::s64 base = linear ? (core::s64(static_cast<core::s32>(data_[25+i])) << (sf ? 12 : 0)) : 0;
        const core::s64 m = sar(data_s16(static_cast<core::u8>(9+i))*ir0 + base, sf ? 12 : 0);
        write_mac(i+1,m);
        data_[9+i] = static_cast<Word>(saturate_ir(m,lm,i+1));
    }
    push_rgb(static_cast<core::s32>(data_[25]), static_cast<core::s32>(data_[26]), static_cast<core::s32>(data_[27]));
}

core::u32 GTE::command_cycles(core::u8 opcode) noexcept
{
    switch (opcode)
    {
    case 0x01: return 15; // RTPS
    case 0x06: return 8;  // NCLIP
    case 0x0C: return 6;  // OP
    case 0x12: return 8;  // MVMVA
    case 0x28: return 5;  // SQR
    case 0x2D: return 5;  // AVSZ3
    case 0x2E: return 6;  // AVSZ4
    case 0x30: return 23; // RTPT
    case 0x3D: return 5;  // GPF
    case 0x3E: return 5;  // GPL
    default: return 1;
    }
}

void GTE::execute(Word instruction) noexcept
{
    if (busy()) return;
    const core::u8 opcode = static_cast<core::u8>(command_opcode(instruction));
    switch (opcode)
    {
    case 0x01: rtps(instruction, false); break;
    case 0x06: nclip(); break;
    case 0x0C: op(instruction); break;
    case 0x12: mvmva(instruction); break;
    case 0x28: sqr(instruction); break;
    case 0x2D: avsz(false); break;
    case 0x2E: avsz(true); break;
    case 0x30: rtps(instruction, true); break;
    case 0x3D: gpf(instruction,false); break;
    case 0x3E: gpf(instruction,true); break;
    default: clear_flag(); break;
    }
    busy_cycles_ = command_cycles(opcode);
}

void GTE::tick() noexcept
{
    if (busy_cycles_ != 0) --busy_cycles_;
}

} // namespace imatfe::gte
