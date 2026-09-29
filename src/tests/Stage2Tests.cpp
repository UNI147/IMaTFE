#include "Stage2Tests.h"

#include "engine/core/Memory.h"
#include "engine/cpu/CPU.h"

#include "TestLog.h"
#include <string>

namespace imatfe::tests
{
namespace
{
struct Runner
{
    int passed = 0;
    int failed = 0;

    void check(bool value, const std::string& name)
    {
        if (value) { ++passed; TestLog::instance() << "  [PASS] " << name << '\n'; }
        else { ++failed; TestLog::instance() << "  [FAIL] " << name << '\n'; }
    }
};

using CPU = imatfe::cpu::CPU;
using Memory = imatfe::core::psx::Memory;
using Word = imatfe::core::u32;

constexpr Word R(Word rs, Word rt, Word rd, Word sh, Word fn)
{
    return (rs << 21) | (rt << 16) | (rd << 11) | (sh << 6) | fn;
}
constexpr Word I(Word op, Word rs, Word rt, Word imm)
{
    return (op << 26) | (rs << 21) | (rt << 16) | (imm & 0xFFFFu);
}
constexpr Word J(Word op, Word target)
{
    return (op << 26) | ((target >> 2) & 0x03FFFFFFu);
}

void put(Memory& memory, Word address, Word instruction)
{
    memory.write32(address, instruction);
}

void test_alu_and_register_zero(Runner& t)
{
    Memory memory;
    CPU cpu(memory);
    cpu.reset(0x80000000u);

    put(memory, 0x00000000u, I(0x09, 0, 1, 5));          // addiu r1,r0,5
    put(memory, 0x00000004u, I(0x09, 1, 2, 7));          // addiu r2,r1,7
    put(memory, 0x00000008u, R(1, 2, 3, 0, 0x21));       // addu r3,r1,r2
    put(memory, 0x0000000Cu, R(3, 0, 0, 0, 0x21));       // addu r0,r3,r0

    cpu.set_reg(0, 0xFFFFFFFFu);
    cpu.step(); cpu.step(); cpu.step(); cpu.step();

    t.check(cpu.reg(1) == 5, "ADDIU writes register");
    t.check(cpu.reg(2) == 12, "ADDIU uses sign-extended immediate");
    t.check(cpu.reg(3) == 17, "ADDU computes register result");
    t.check(cpu.reg(0) == 0, "R0 remains hard-wired to zero");
}

void test_branch_delay_and_jal(Runner& t)
{
    Memory memory;
    CPU cpu(memory);
    cpu.reset(0x80000000u);

    put(memory, 0x00000000u, I(0x09, 0, 1, 1)); // r1=1
    put(memory, 0x00000004u, I(0x04, 1, 1, 2)); // beq r1,r1,+2 -> 0x10
    put(memory, 0x00000008u, I(0x09, 1, 2, 4)); // delay slot: r2=5
    put(memory, 0x0000000Cu, I(0x09, 0, 3, 9)); // skipped
    put(memory, 0x00000010u, I(0x09, 2, 3, 3)); // r3=8

    cpu.step();
    cpu.step();
    t.check(cpu.state().pc == 0x80000008u, "Taken branch enters its delay slot");
    cpu.step();
    t.check(cpu.state().pc == 0x80000010u, "Branch target is entered after delay slot");
    cpu.step();
    t.check(cpu.reg(2) == 5 && cpu.reg(3) == 8, "Delay slot executes and branch target executes");

    Memory memory2;
    CPU cpu2(memory2);
    cpu2.reset(0x80000000u);
    put(memory2, 0, J(0x03, 0x80000010u)); // jal 0x10
    put(memory2, 4, I(0x09, 0, 2, 7));       // delay slot
    put(memory2, 0x10, I(0x09, 0, 3, 9));
    cpu2.step();
    t.check(cpu2.reg(31) == 0x80000008u, "JAL stores PC+8 in RA");
    cpu2.step(); cpu2.step();
    t.check(cpu2.reg(2) == 7 && cpu2.reg(3) == 9, "JAL delay slot is executed");
}

void test_load_delay(Runner& t)
{
    Memory memory;
    CPU cpu(memory);
    cpu.reset(0x80000000u);
    memory.write32(0x00000100u, 0x12345678u);

    put(memory, 0x00000000u, I(0x09, 0, 4, 0x100)); // r4=100h
    put(memory, 0x00000004u, I(0x23, 4, 1, 0));      // lw r1,0(r4)
    put(memory, 0x00000008u, R(1, 0, 2, 0, 0x21));   // addu r2,r1,r0
    put(memory, 0x0000000Cu, R(1, 0, 3, 0, 0x21));   // addu r3,r1,r0

    cpu.step(); cpu.step();
    t.check(cpu.reg(1) == 0, "Load result is not visible immediately");
    cpu.step();
    t.check(cpu.reg(2) == 0 && cpu.reg(1) == 0x12345678u, "Load delay hides value from the following instruction");
    cpu.step();
    t.check(cpu.reg(3) == 0x12345678u, "Loaded value is visible after the delay slot");
}

void test_overflow_and_exceptions(Runner& t)
{
    Memory memory;
    CPU cpu(memory);
    cpu.reset(0x80000000u);
    cpu.state().status = 0; // BEV=0 for the RAM exception vector.

    put(memory, 0, I(0x0F, 0, 1, 0x7FFF)); // lui r1,7fff
    put(memory, 4, I(0x0D, 1, 1, 0xffff)); // ori r1,r1,ffff -> 7fffffff
    put(memory, 8, I(0x08, 1, 2, 1));      // addi r2,r1,1 -> overflow
    cpu.step(); cpu.step();

    bool trapped = false;
    try { cpu.step(); }
    catch (const std::runtime_error&) { trapped = true; }

    t.check(trapped, "ADDI overflow raises an exception");
    t.check(((cpu.state().cause >> 2) & 0x1Fu) == 12, "Overflow exception code is 0Ch");
    t.check(cpu.state().epc == 0x80000008u, "EPC points at the faulting instruction");
    t.check(cpu.state().pc == 0x80000080u, "BEV=0 exception enters 80000080h");
    t.check(cpu.reg(2) == 0, "Faulting ADDI leaves destination unchanged");

    Memory memory2;
    CPU cpu2(memory2);
    cpu2.reset(0x80000000u);
    cpu2.state().status = 0;
    put(memory2, 0, I(0x04, 0, 0, 1)); // beq r0,r0,+1
    put(memory2, 4, 0x0000000Cu);       // syscall in delay slot
    cpu2.step();
    bool trapped2 = false;
    try { cpu2.step(); } catch (const std::runtime_error&) { trapped2 = true; }
    t.check(trapped2, "Exception in a branch delay slot traps");
    t.check((cpu2.state().cause & 0x80000000u) != 0, "BD is set for an exception in a delay slot");
    t.check(cpu2.state().epc == 0x80000000u, "Delay-slot exception EPC points to the branch");
}

void test_cop0(Runner& t)
{
    Memory memory;
    CPU cpu(memory);
    cpu.reset(0x80000000u);
    cpu.state().status = 0;

    put(memory, 0, (0x10u << 26) | (0u << 21) | (1u << 16) | (12u << 11)); // mfc0 r1,sr
    put(memory, 4, R(0, 0, 0, 0, 0));
    put(memory, 8, (0x10u << 26) | (4u << 21) | (1u << 16) | (12u << 11)); // mtc0 r1,sr
    put(memory, 12, (0x10u << 26) | (16u << 21) | 0x10u); // rfe

    cpu.set_reg(1, 0x0000000Fu);
    cpu.step();
    t.check(cpu.reg(1) == 0x0000000Fu, "MFC0 leaves the old GPR value visible during the delay");
    cpu.step();
    t.check(cpu.reg(1) == 0 && cpu.state().status == 0, "MFC0 result commits after the following instruction");

    cpu.set_reg(1, 0x00000001u);
    cpu.step();
    t.check(cpu.state().status == 0x01u, "MTC0 writes COP0 status");
    cpu.step();
    t.check((cpu.state().status & 0x3Fu) == 0x00u, "RFE pops the low interrupt/mode status stack");
}

} // namespace

bool run_stage2_tests()
{
    TestLog::instance() << "\n=== IMaTFE Stage 2 Verification ===\n";
    TestLog::instance() << "R3000A Interpreter / Decode / Branches / Delay Slots / Exceptions / COP0\n";

    Runner r;
    test_alu_and_register_zero(r);
    test_branch_delay_and_jal(r);
    test_load_delay(r);
    test_overflow_and_exceptions(r);
    test_cop0(r);

    TestLog::instance() << "\n=== Stage 2 Test Summary ===\n";
    TestLog::instance() << "Passed: " << r.passed << '\n';
    TestLog::instance() << "Failed: " << r.failed << '\n';
    return r.failed == 0;
}

} // namespace imatfe::tests
