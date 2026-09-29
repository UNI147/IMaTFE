#pragma once

#include "engine/core/Memory.h"
#include "engine/core/Types.h"

#include <array>
#include <optional>

namespace imatfe::cpu
{

class CPU final
{
public:
    using Word = core::u32;
    using Reg = core::u8;

    enum class ExceptionCode : core::u32
    {
        Interrupt = 0,
        AdEL = 4,
        AdES = 5,
        BusErrorInstruction = 6,
        BusErrorData = 7,
        Syscall = 8,
        Breakpoint = 9,
        ReservedInstruction = 10,
        CoprocessorUnusable = 11,
        ArithmeticOverflow = 12,
    };

    struct State
    {
        std::array<Word, 32> gpr{};
        Word hi = 0;
        Word lo = 0;
        Word pc = 0;

        Word bad_vaddr = 0;
        Word status = 0;
        Word cause = 0;
        Word epc = 0;
        Word prid = 0x00000002u;

        Word bpc = 0;
        Word bda = 0;
        Word dcic = 0;
        Word bdam = 0;
        Word bpcm = 0;
    };

    explicit CPU(core::psx::Memory& memory);

    void reset(Word pc = 0xBFC00000u);
    void step();

    const State& state() const noexcept { return state_; }
    State& state() noexcept { return state_; }

    Word reg(Reg index) const noexcept;
    void set_reg(Reg index, Word value) noexcept;

    bool halted() const noexcept { return halted_; }
    void clear_halt() noexcept { halted_ = false; }

private:
    struct PendingLoad { Reg reg; Word value; };

    core::psx::Memory& memory_;
    State state_{};
    bool halted_ = false;

    std::optional<PendingLoad> pending_load_;
    bool branch_pending_ = false;
    Word branch_target_ = 0;
    bool in_delay_slot_ = false;
    Word delay_branch_pc_ = 0;

    static core::s32 sign_extend16(core::u16 value) noexcept;
    static Word branch_target(Word pc, core::u16 imm) noexcept;
    static Word jump_target(Word pc, core::u32 imm26) noexcept;

    Word read_reg(Reg index) const noexcept;
    void write_reg(Reg index, Word value) noexcept;
    void schedule_load(Reg index, Word value) noexcept;
    void commit_pending_load() noexcept;

    void execute(Word instruction);
    void execute_special(Word instruction);
    void execute_regimm(Word instruction);
    void execute_cop0(Word instruction);
    void execute_cop2(Word instruction);
    void execute_load_store(Word instruction);
    void execute_branch(Word instruction, bool condition, bool link);

    void schedule_branch(Word target) noexcept;
    [[noreturn]] void raise_exception(ExceptionCode code, Word bad_address = 0,
                                      bool has_bad_address = false);

    Word read_memory8(Word address);
    Word read_memory16(Word address);
    Word read_memory32(Word address);
    void write_memory8(Word address, Word value);
    void write_memory16(Word address, Word value);
    void write_memory32(Word address, Word value);

    Word cop0_read(core::u8 index) const noexcept;
    void cop0_write(core::u8 index, Word value) noexcept;
    void do_rfe() noexcept;
};

} // namespace imatfe::cpu
