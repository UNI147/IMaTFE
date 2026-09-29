#include "CPU.h"

#include <limits>
#include <stdexcept>
#include <string>

namespace imatfe::cpu
{

CPU::CPU(core::psx::Memory& memory) : memory_(memory)
{
    reset();
}

void CPU::reset(Word pc)
{
    state_ = {};
    state_.pc = pc;
    state_.prid = 0x00000002u;
    // PSX boot starts in kernel mode with interrupts disabled and BEV set.
    state_.status = (1u << 22);
    halted_ = false;
    pending_load_.reset();
    branch_pending_ = false;
    branch_target_ = 0;
    in_delay_slot_ = false;
    delay_branch_pc_ = 0;
}

CPU::Word CPU::reg(Reg index) const noexcept
{
    return read_reg(index);
}

void CPU::set_reg(Reg index, Word value) noexcept
{
    write_reg(index, value);
}

CPU::Word CPU::read_reg(Reg index) const noexcept
{
    return index == 0 ? 0u : state_.gpr[index];
}

void CPU::write_reg(Reg index, Word value) noexcept
{
    if (index != 0)
        state_.gpr[index] = value;
    state_.gpr[0] = 0;
}

void CPU::schedule_load(Reg index, Word value) noexcept
{
    if (index == 0)
        return;
    pending_load_ = PendingLoad{index, value};
}

void CPU::commit_pending_load() noexcept
{
    if (!pending_load_)
        return;
    write_reg(pending_load_->reg, pending_load_->value);
    pending_load_.reset();
}

core::s32 CPU::sign_extend16(core::u16 value) noexcept
{
    return static_cast<core::s32>(static_cast<core::s16>(value));
}

CPU::Word CPU::branch_target(Word pc, core::u16 imm) noexcept
{
    return pc + 4u + (static_cast<Word>(static_cast<core::s32>(sign_extend16(imm))) << 2);
}

CPU::Word CPU::jump_target(Word pc, core::u32 imm26) noexcept
{
    return (pc & 0xF0000000u) | (imm26 << 2);
}

void CPU::schedule_branch(Word target) noexcept
{
    branch_pending_ = true;
    branch_target_ = target;
}

[[noreturn]] void CPU::raise_exception(ExceptionCode code, Word bad_address,
                                       bool has_bad_address)
{
    const Word old_pc = in_delay_slot_ ? delay_branch_pc_ : state_.pc;
    const bool bd = in_delay_slot_;

    state_.cause &= ~((0x1Fu << 2) | (3u << 28) | (1u << 30) | (1u << 31));
    state_.cause |= (static_cast<Word>(code) & 0x1Fu) << 2;
    if (bd)
        state_.cause |= 1u << 31;

    if (has_bad_address &&
        (code == ExceptionCode::AdEL || code == ExceptionCode::AdES))
        state_.bad_vaddr = bad_address;

    state_.epc = old_pc;

    // Exception entry shifts IE/KU: current -> previous, previous -> old.
    const Word mode = state_.status & 0x3Fu;
    state_.status = (state_.status & ~0x3Fu) |
                    ((mode << 2) & 0x3Fu);

    const Word vector = (state_.status & (1u << 22)) ? 0xBFC00180u : 0x80000080u;
    state_.pc = vector;
    branch_pending_ = false;
    in_delay_slot_ = false;
    pending_load_.reset();
    state_.gpr[0] = 0;
    throw std::runtime_error("CPU exception");
}

CPU::Word CPU::read_memory8(Word address)
{
    try { return memory_.read8(address); }
    catch (const std::runtime_error&) { raise_exception(ExceptionCode::AdEL, address, true); }
    catch (const std::out_of_range&) { raise_exception(ExceptionCode::BusErrorData); }
}

CPU::Word CPU::read_memory16(Word address)
{
    try { return memory_.read16(address); }
    catch (const std::runtime_error&) { raise_exception(ExceptionCode::AdEL, address, true); }
    catch (const std::out_of_range&) { raise_exception(ExceptionCode::BusErrorData); }
}

CPU::Word CPU::read_memory32(Word address)
{
    try { return memory_.read32(address); }
    catch (const std::runtime_error&) { raise_exception(ExceptionCode::AdEL, address, true); }
    catch (const std::out_of_range&) { raise_exception(ExceptionCode::BusErrorData); }
}

void CPU::write_memory8(Word address, Word value)
{
    try { memory_.write8(address, static_cast<core::u8>(value)); }
    catch (const std::runtime_error&) { raise_exception(ExceptionCode::AdES, address, true); }
    catch (const std::out_of_range&) { raise_exception(ExceptionCode::BusErrorData); }
}

void CPU::write_memory16(Word address, Word value)
{
    try { memory_.write16(address, static_cast<core::u16>(value)); }
    catch (const std::runtime_error&) { raise_exception(ExceptionCode::AdES, address, true); }
    catch (const std::out_of_range&) { raise_exception(ExceptionCode::BusErrorData); }
}

void CPU::write_memory32(Word address, Word value)
{
    try { memory_.write32(address, value); }
    catch (const std::runtime_error&) { raise_exception(ExceptionCode::AdES, address, true); }
    catch (const std::out_of_range&) { raise_exception(ExceptionCode::BusErrorData); }
}

void CPU::step()
{
    if (halted_)
        return;

    // A pending load becomes visible only after the following instruction has
    // completed. Therefore the instruction currently executing still observes
    // the old GPR value.
    const auto load_from_previous = pending_load_;
    pending_load_.reset();

    const bool delay_slot = in_delay_slot_;
    const Word current_pc = state_.pc;

    Word instruction = 0;
    try
    {
        instruction = memory_.read32(current_pc);
    }
    catch (const std::runtime_error&)
    {
        in_delay_slot_ = delay_slot;
        raise_exception(ExceptionCode::AdEL, current_pc, true);
    }
    catch (const std::out_of_range&)
    {
        in_delay_slot_ = delay_slot;
        raise_exception(ExceptionCode::BusErrorInstruction);
    }

    in_delay_slot_ = delay_slot;
    try
    {
        execute(instruction);
    }
    catch (const std::runtime_error& e)
    {
        // Architectural CPU exceptions are raised through raise_exception().
        // Other runtime errors are allowed to surface to the caller.
        if (std::string(e.what()) == "CPU exception")
            throw;
        throw;
    }

    // The load completes after the instruction following it has executed.
    if (load_from_previous)
    {
        write_reg(load_from_previous->reg, load_from_previous->value);
    }

    if (delay_slot)
    {
        state_.pc = branch_target_;
        branch_pending_ = false;
        in_delay_slot_ = false;
    }
    else
    {
        state_.pc = current_pc + 4u;
        if (branch_pending_)
        {
            in_delay_slot_ = true;
            delay_branch_pc_ = current_pc;
        }
    }

    state_.gpr[0] = 0;
}

void CPU::execute(Word instruction)
{
    const core::u32 op = instruction >> 26;
    switch (op)
    {
    case 0x00: execute_special(instruction); break;
    case 0x01: execute_regimm(instruction); break;
    case 0x02: schedule_branch(jump_target(state_.pc, instruction & 0x03FFFFFFu)); break;
    case 0x03:
        write_reg(31, state_.pc + 8u);
        schedule_branch(jump_target(state_.pc, instruction & 0x03FFFFFFu));
        break;
    case 0x04: execute_branch(instruction, read_reg((instruction >> 21) & 31) == read_reg((instruction >> 16) & 31), false); break;
    case 0x05: execute_branch(instruction, read_reg((instruction >> 21) & 31) != read_reg((instruction >> 16) & 31), false); break;
    case 0x06: execute_branch(instruction, static_cast<core::s32>(read_reg((instruction >> 21) & 31)) <= 0, false); break;
    case 0x07: execute_branch(instruction, static_cast<core::s32>(read_reg((instruction >> 21) & 31)) > 0, false); break;
    case 0x08:
    {
        const Reg rs = (instruction >> 21) & 31, rt = (instruction >> 16) & 31;
        const auto a = static_cast<core::s64>(static_cast<core::s32>(read_reg(rs)));
        const auto b = static_cast<core::s64>(sign_extend16(static_cast<core::u16>(instruction)));
        const auto r = a + b;
        if (r < std::numeric_limits<core::s32>::min() || r > std::numeric_limits<core::s32>::max())
            raise_exception(ExceptionCode::ArithmeticOverflow);
        write_reg(rt, static_cast<Word>(static_cast<core::s32>(r)));
        break;
    }
    case 0x09: write_reg((instruction >> 16) & 31, read_reg((instruction >> 21) & 31) + static_cast<Word>(sign_extend16(static_cast<core::u16>(instruction)))); break;
    case 0x0A: write_reg((instruction >> 16) & 31, static_cast<core::s32>(read_reg((instruction >> 21) & 31)) < sign_extend16(static_cast<core::u16>(instruction))); break;
    case 0x0B: write_reg((instruction >> 16) & 31, read_reg((instruction >> 21) & 31) < static_cast<Word>(sign_extend16(static_cast<core::u16>(instruction)))); break;
    case 0x0C: write_reg((instruction >> 16) & 31, read_reg((instruction >> 21) & 31) & (instruction & 0xFFFFu)); break;
    case 0x0D: write_reg((instruction >> 16) & 31, read_reg((instruction >> 21) & 31) | (instruction & 0xFFFFu)); break;
    case 0x0E: write_reg((instruction >> 16) & 31, read_reg((instruction >> 21) & 31) ^ (instruction & 0xFFFFu)); break;
    case 0x0F: write_reg((instruction >> 16) & 31, (instruction & 0xFFFFu) << 16); break;
    case 0x10: execute_cop0(instruction); break;
    case 0x12: execute_cop2(instruction); break;
    case 0x20: case 0x21: case 0x22: case 0x23: case 0x24: case 0x25: case 0x26: case 0x27:
    case 0x28: case 0x29: case 0x2A: case 0x2E:
        execute_load_store(instruction); break;
    default: raise_exception(ExceptionCode::ReservedInstruction);
    }
}

void CPU::execute_special(Word instruction)
{
    const Reg rs = (instruction >> 21) & 31, rt = (instruction >> 16) & 31, rd = (instruction >> 11) & 31;
    const core::u32 sa = (instruction >> 6) & 31;
    const core::u32 fn = instruction & 63;

    switch (fn)
    {
    case 0x00: write_reg(rd, read_reg(rt) << sa); break;
    case 0x02: write_reg(rd, read_reg(rt) >> sa); break;
    case 0x03: write_reg(rd, static_cast<Word>(static_cast<core::s32>(read_reg(rt)) >> sa)); break;
    case 0x04: write_reg(rd, read_reg(rt) << (read_reg(rs) & 31)); break;
    case 0x06: write_reg(rd, read_reg(rt) >> (read_reg(rs) & 31)); break;
    case 0x07: write_reg(rd, static_cast<Word>(static_cast<core::s32>(read_reg(rt)) >> (read_reg(rs) & 31))); break;
    case 0x08:
        schedule_branch(read_reg(rs));
        break;
    case 0x09:
    {
        const Word target = read_reg(rs);
        write_reg(rd, state_.pc + 8u);
        schedule_branch(target);
        break;
    }
    case 0x0C: raise_exception(ExceptionCode::Syscall); break;
    case 0x0D: raise_exception(ExceptionCode::Breakpoint); break;
    case 0x10: write_reg(rd, state_.hi); break;
    case 0x11: state_.hi = read_reg(rs); break;
    case 0x12: write_reg(rd, state_.lo); break;
    case 0x13: state_.lo = read_reg(rs); break;
    case 0x18:
    {
        const core::s64 result = static_cast<core::s64>(static_cast<core::s32>(read_reg(rs))) * static_cast<core::s64>(static_cast<core::s32>(read_reg(rt)));
        state_.lo = static_cast<Word>(result);
        state_.hi = static_cast<Word>(result >> 32);
        break;
    }
    case 0x19:
    {
        const core::u64 result = static_cast<core::u64>(read_reg(rs)) * static_cast<core::u64>(read_reg(rt));
        state_.lo = static_cast<Word>(result);
        state_.hi = static_cast<Word>(result >> 32);
        break;
    }
    case 0x1A:
    {
        const core::s32 a = static_cast<core::s32>(read_reg(rs));
        const core::s32 b = static_cast<core::s32>(read_reg(rt));
        if (b == 0)
        {
            state_.lo = (a >= 0) ? 0xFFFFFFFFu : 1u;
            state_.hi = static_cast<Word>(a);
        }
        else if (a == std::numeric_limits<core::s32>::min() && b == -1)
        {
            state_.lo = 0x80000000u;
            state_.hi = 0;
        }
        else
        {
            state_.lo = static_cast<Word>(a / b);
            state_.hi = static_cast<Word>(a % b);
        }
        break;
    }
    case 0x1B:
    {
        const Word a = read_reg(rs), b = read_reg(rt);
        state_.lo = b == 0 ? 0xFFFFFFFFu : a / b;
        state_.hi = b == 0 ? a : a % b;
        break;
    }
    case 0x20:
    {
        const auto r = static_cast<core::s64>(static_cast<core::s32>(read_reg(rs))) + static_cast<core::s64>(static_cast<core::s32>(read_reg(rt)));
        if (r < std::numeric_limits<core::s32>::min() || r > std::numeric_limits<core::s32>::max()) raise_exception(ExceptionCode::ArithmeticOverflow);
        write_reg(rd, static_cast<Word>(static_cast<core::s32>(r)));
        break;
    }
    case 0x21: write_reg(rd, read_reg(rs) + read_reg(rt)); break;
    case 0x22:
    {
        const auto r = static_cast<core::s64>(static_cast<core::s32>(read_reg(rs))) - static_cast<core::s64>(static_cast<core::s32>(read_reg(rt)));
        if (r < std::numeric_limits<core::s32>::min() || r > std::numeric_limits<core::s32>::max()) raise_exception(ExceptionCode::ArithmeticOverflow);
        write_reg(rd, static_cast<Word>(static_cast<core::s32>(r)));
        break;
    }
    case 0x23: write_reg(rd, read_reg(rs) - read_reg(rt)); break;
    case 0x24: write_reg(rd, read_reg(rs) & read_reg(rt)); break;
    case 0x25: write_reg(rd, read_reg(rs) | read_reg(rt)); break;
    case 0x26: write_reg(rd, read_reg(rs) ^ read_reg(rt)); break;
    case 0x27: write_reg(rd, ~(read_reg(rs) | read_reg(rt))); break;
    case 0x2A: write_reg(rd, static_cast<core::s32>(read_reg(rs)) < static_cast<core::s32>(read_reg(rt))); break;
    case 0x2B: write_reg(rd, read_reg(rs) < read_reg(rt)); break;
    default: raise_exception(ExceptionCode::ReservedInstruction);
    }
}

void CPU::execute_regimm(Word instruction)
{
    const Reg rs = (instruction >> 21) & 31;
    const core::u32 rt = (instruction >> 16) & 31;
    const bool negative = static_cast<core::s32>(read_reg(rs)) < 0;
    const bool zero_or_positive = !negative;
    const core::u32 fn = rt & 0x1Fu;

    switch (fn)
    {
    case 0x00: execute_branch(instruction, negative, false); break;
    case 0x01: execute_branch(instruction, zero_or_positive, false); break;
    case 0x10: execute_branch(instruction, negative, true); break;
    case 0x11: execute_branch(instruction, zero_or_positive, true); break;
    default: raise_exception(ExceptionCode::ReservedInstruction);
    }
}

void CPU::execute_branch(Word instruction, bool condition, bool link)
{
    if (link)
        write_reg(31, state_.pc + 8u);
    if (condition)
        schedule_branch(branch_target(state_.pc, static_cast<core::u16>(instruction)));
    else
        schedule_branch(state_.pc + 4u);
}

CPU::Word CPU::cop0_read(core::u8 index) const noexcept
{
    switch (index)
    {
    case 3: return state_.bpc;
    case 5: return state_.bda;
    case 6: return state_.bpc;
    case 7: return state_.dcic;
    case 8: return state_.bad_vaddr;
    case 9: return state_.bdam;
    case 11: return state_.bpcm;
    case 12: return state_.status;
    case 13: return state_.cause;
    case 14: return state_.epc;
    case 15: return state_.prid;
    default: return 0;
    }
}

void CPU::cop0_write(core::u8 index, Word value) noexcept
{
    switch (index)
    {
    case 3: state_.bpc = value; break;
    case 5: state_.bda = value; break;
    case 7: state_.dcic = value; break;
    case 9: state_.bdam = value; break;
    case 11: state_.bpcm = value; break;
    case 12: state_.status = value; break;
    case 13: state_.cause = (state_.cause & ~0x300u) | (value & 0x300u); break;
    case 14: state_.epc = value; break;
    default: break;
    }
}

void CPU::do_rfe() noexcept
{
    const Word sr = state_.status;
    state_.status = (sr & ~0x3Fu) | ((sr >> 2) & 0x0Fu) | (sr & 0x30u);
}

void CPU::execute_cop0(Word instruction)
{
    const core::u32 rs = (instruction >> 21) & 31;
    const Reg rt = (instruction >> 16) & 31;
    const core::u8 rd = static_cast<core::u8>((instruction >> 11) & 31);

    // CU0 permits COP0 only in user mode; PSX uses it primarily in kernel mode.
    const bool user = (state_.status & 0x2u) != 0;
    const bool enabled = !user || (state_.status & (1u << 28));
    if (!enabled)
        raise_exception(ExceptionCode::CoprocessorUnusable);

    switch (rs)
    {
    case 0x00: schedule_load(rt, cop0_read(rd)); break; // MFC0
    case 0x04: cop0_write(rd, read_reg(rt)); break;      // MTC0
    case 0x10:
        if ((instruction & 0x3Fu) == 0x10u)
            do_rfe();
        else
            raise_exception(ExceptionCode::ReservedInstruction);
        break;
    default:
        raise_exception(ExceptionCode::ReservedInstruction);
    }
}

void CPU::execute_cop2(Word instruction)
{
    if ((state_.status & (1u << 30)) == 0)
        raise_exception(ExceptionCode::CoprocessorUnusable);

    // GTE execution is a separate IMaTFE stage. Keep the CPU-side COP2
    // encoding/availability behavior here; actual COP2 register semantics are
    // supplied when the GTE subsystem is introduced.
    (void)instruction;
    raise_exception(ExceptionCode::ReservedInstruction);
}

void CPU::execute_load_store(Word instruction)
{
    const core::u32 op = instruction >> 26;
    const Reg rs = (instruction >> 21) & 31;
    const Reg rt = (instruction >> 16) & 31;
    const Word address = read_reg(rs) + static_cast<Word>(sign_extend16(static_cast<core::u16>(instruction)));

    switch (op)
    {
    case 0x20: schedule_load(rt, static_cast<Word>(static_cast<core::s32>(static_cast<core::s8>(read_memory8(address))))); break;
    case 0x24: schedule_load(rt, read_memory8(address)); break;
    case 0x21: schedule_load(rt, static_cast<Word>(static_cast<core::s32>(static_cast<core::s16>(read_memory16(address))))); break;
    case 0x25: schedule_load(rt, read_memory16(address)); break;
    case 0x23: schedule_load(rt, read_memory32(address)); break;
    case 0x28: write_memory8(address, read_reg(rt)); break;
    case 0x29: write_memory16(address, read_reg(rt)); break;
    case 0x2B: write_memory32(address, read_reg(rt)); break;
    case 0x26: // LWR
    {
        const Word mem = read_memory32(address & ~3u);
        const unsigned k = address & 3u;
        const Word mask = 0xFFFFFF00u << ((3u - k) * 8u);
        const Word value = (read_reg(rt) & mask) | (mem >> (k * 8u));
        schedule_load(rt, value);
        break;
    }
    case 0x22: // LWL
    {
        const Word mem = read_memory32(address & ~3u);
        const unsigned k = address & 3u;
        const Word mask = 0x00FFFFFFu >> (k * 8u);
        const Word value = (read_reg(rt) & mask) | (mem << ((3u - k) * 8u));
        schedule_load(rt, value);
        break;
    }
    case 0x2E: // SWR
    {
        const Word old = read_memory32(address & ~3u);
        const unsigned k = address & 3u;
        const Word mask = 0xFFFFFFFFu << ((3u - k) * 8u);
        const Word value = (old & mask) | (read_reg(rt) << ((3u - k) * 8u));
        write_memory32(address & ~3u, value);
        break;
    }
    case 0x2A: // SWL
    {
        const Word old = read_memory32(address & ~3u);
        const unsigned k = address & 3u;
        const Word mask = 0xFFFFFFFFu >> ((3u - k) * 8u);
        const Word value = (old & ~mask) | (read_reg(rt) >> ((3u - k) * 8u));
        write_memory32(address & ~3u, value);
        break;
    }
    default: raise_exception(ExceptionCode::ReservedInstruction);
    }
}

} // namespace imatfe::cpu
