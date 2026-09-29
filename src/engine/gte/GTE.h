#pragma once

#include "engine/core/Types.h"

#include <array>

namespace imatfe::gte
{

class GTE final
{
public:
    using Word = core::u32;

    GTE();

    void reset() noexcept;

    Word read_data(core::u8 index) const noexcept;
    void write_data(core::u8 index, Word value) noexcept;

    Word read_control(core::u8 index) const noexcept;
    void write_control(core::u8 index, Word value) noexcept;

    void execute(Word instruction) noexcept;

    // One CPU clock worth of GTE execution.  The result of a command is
    // committed when busy reaches zero.  Register writes remain possible
    // while a command is running, as on the original interlocked COP2 unit.
    void tick() noexcept;
    bool busy() const noexcept { return busy_cycles_ != 0; }
    core::u32 busy_cycles() const noexcept { return busy_cycles_; }

    // A CPU-side interlock is required for MFC2/CFC2/SWC2 and for the next
    // COP2 command.  MTC2/CTC2/LWC2 are non-blocking.
    bool command_read_hazard() const noexcept { return busy(); }

    core::u32 flag() const noexcept { return control_[31]; }

private:
    std::array<Word, 32> data_{};
    std::array<Word, 32> control_{};
    core::u32 busy_cycles_ = 0;

    static core::s16 lo16(Word value) noexcept;
    static core::s16 hi16(Word value) noexcept;
    static Word pack16(core::s16 lo, core::s16 hi) noexcept;
    static core::s32 sext16(core::u16 value) noexcept;
    static core::s64 sar(core::s64 value, unsigned bits) noexcept;

    core::s32 data_s16(core::u8 index) const noexcept;
    core::s32 control_s16(core::u8 index) const noexcept;
    core::u16 data_u16(core::u8 index) const noexcept;

    void clear_flag() noexcept;
    void set_flag(core::u32 bits) noexcept;
    void write_mac(int index, core::s64 value) noexcept;
    void write_mac0(core::s64 value) noexcept;

    core::s32 saturate_ir(core::s64 value, bool lm, int index) noexcept;
    core::u16 saturate_sz(core::s64 value) noexcept;
    core::s16 saturate_screen(core::s64 value, bool y) noexcept;
    core::u16 saturate_otz(core::s64 value) noexcept;
    core::u16 irgb_component(core::s32 value) const noexcept;
    core::u32 read_lzcr() const noexcept;

    core::s64 matrix_element(core::u8 mx, int row, int col) const noexcept;
    core::s64 vector_element(core::u8 v, int index) const noexcept;
    core::s64 constant_element(core::u8 cv, int index) const noexcept;

    void push_sxy(core::s16 x, core::s16 y) noexcept;
    void push_sz(core::u16 z) noexcept;
    void push_rgb(core::s64 mac1, core::s64 mac2, core::s64 mac3) noexcept;

    void mvmva(Word instruction) noexcept;
    void rtps(Word instruction, bool triple) noexcept;
    void transform_vertex(int vertex, Word instruction, core::s16& sx, core::s16& sy, core::u16& sz) noexcept;
    void nclip() noexcept;
    void avsz(bool four) noexcept;
    void op(Word instruction) noexcept;
    void sqr(Word instruction) noexcept;
    void gpf(Word instruction, bool linear) noexcept;

    static core::u32 command_cycles(core::u8 opcode) noexcept;
};

} // namespace imatfe::gte
