#include <stdint.h>
#include <math.h>
#include <assert.h>
#include <stdbool.h>

#include <stdio.h>

#include "const.h"
#include "global_pointers.h"

static int32_t get_stack_pointer(int32_t);
static void adjust_stack_reg(int32_t);
void branch_taken();
void branch_not_taken();
void writable_or_pagefault(int32_t, int32_t);


static void write_reg8(int32_t index, int32_t value);
static int32_t read_reg16(int32_t index);
static void write_reg16(int32_t index, int32_t value);
static int32_t read_reg32(int32_t index);
static void write_reg32(int32_t index, int32_t value);


int32_t getcf()
{
    if(*flags_changed & 1)
    {
        return (*last_op1 ^ (*last_op1 ^ *last_op2) & (*last_op2 ^ *last_add_result)) >> *last_op_size & 1;
    }
    else
    {
        return *flags & 1;
    }
}

int32_t getpf()
{
    if(*flags_changed & FLAG_PARITY)
    {
        // inverted lookup table
        return 0x9669 << 2 >> ((*last_result ^ *last_result >> 4) & 0xF) & FLAG_PARITY;
    }
    else
    {
        return *flags & FLAG_PARITY;
    }
}

int32_t getaf()
{
    if(*flags_changed & FLAG_ADJUST)
    {
        return (*last_op1 ^ *last_op2 ^ *last_add_result) & FLAG_ADJUST;
    }
    else
    {
        return *flags & FLAG_ADJUST;
    }
}

int32_t getzf()
{
    if(*flags_changed & FLAG_ZERO)
    {
        return (~*last_result & *last_result - 1) >> *last_op_size & 1;
    }
    else
    {
        return *flags & FLAG_ZERO;
    }
}

int32_t getsf()
{
    if(*flags_changed & FLAG_SIGN)
    {
        return *last_result >> *last_op_size & 1;
    }
    else
    {
        return *flags & FLAG_SIGN;
    }
}

int32_t getof()
{
    if(*flags_changed & FLAG_OVERFLOW)
    {
        return ((*last_op1 ^ *last_add_result) & (*last_op2 ^ *last_add_result)) >> *last_op_size & 1;
    }
    else
    {
        return *flags & FLAG_OVERFLOW;
    }
}

int32_t test_o() { return getof(); }
int32_t test_b() { return getcf(); }
int32_t test_z() { return getzf(); }
int32_t test_s() { return getsf(); }
int32_t test_p() { return getpf(); }
int32_t test_be() { return getcf() || getzf(); }
int32_t test_l() { return !getsf() != !getof(); }
int32_t test_le() { return getzf() || !getsf() != !getof(); }

void jmp_rel16(int32_t rel16)
{
    int32_t cs_offset = get_seg(CS);

    // limit ip to 16 bit
    *instruction_pointer = cs_offset + ((*instruction_pointer - cs_offset + rel16) & 0xFFFF);
    branch_taken();
}

void jmpcc8(bool condition, int32_t imm8)
{
    if(condition)
    {
        *instruction_pointer += imm8;
        branch_taken();
    }
    else
    {
        branch_not_taken();
    }
}

void jmpcc16(bool condition, int32_t imm16)
{
    if(condition)
    {
        jmp_rel16(imm16);
        branch_taken();
    }
    else
    {
        branch_not_taken();
    }
}

void jmpcc32(bool condition, int32_t imm32)
{
    if(condition)
    {
        *instruction_pointer += imm32;
        branch_taken();
    }
    else
    {
        branch_not_taken();
    }
}

static void cmovcc16(bool condition, int32_t value, int32_t r)
{
    if(condition)
    {
        write_reg16(r, value);
    }
}

static void cmovcc32(bool condition, int32_t value, int32_t r)
{
    if(condition)
    {
        write_reg32(r, value);
    }
}

static int32_t get_stack_pointer(int32_t offset)
{
    if(*stack_size_32)
    {
        return get_seg(SS) + reg32s[ESP] + offset;
    }
    else
    {
        return get_seg(SS) + (reg16[SP] + offset & 0xFFFF);
    }
}

static void adjust_stack_reg(int32_t adjustment)
{
    if(*stack_size_32)
    {
        reg32s[ESP] += adjustment;
    }
    else
    {
        reg16[SP] += adjustment;
    }
}

void push16(int32_t imm16)
{
    if(*stack_size_32)
    {
        int32_t sp = get_seg(SS) + reg32s[ESP] - 2;
        safe_write16(sp, imm16);
        reg32s[ESP] += -2;
    }
    else
    {
        int32_t sp = get_seg(SS) + (reg16[SP] - 2 & 0xFFFF);
        safe_write16(sp, imm16);
        reg16[SP] += -2;
    }
}

void push32(int32_t imm32)
{
    if(*stack_size_32)
    {
        int32_t sp = get_seg(SS) + reg32s[ESP] - 4;
        safe_write32(sp, imm32);
        reg32s[ESP] += -4;
    }
    else
    {
        int32_t sp = get_seg(SS) + (reg16[SP] - 4 & 0xFFFF);
        safe_write32(sp, imm32);
        reg16[SP] += -4;
    }
}

int32_t pop16()
{
    int32_t sp = get_seg(SS) + get_stack_reg();
    int32_t result = safe_read16(sp);

    adjust_stack_reg(2);
    return result;
}

int32_t pop32s()
{
    if(*stack_size_32)
    {
        int32_t sp = get_seg(SS) + reg32s[ESP];
        int32_t result = safe_read32s(sp);
        reg32s[ESP] += 4;
        return result;
    }
    else
    {
        int32_t sp = get_seg(SS) + reg16[SP];
        int32_t result = safe_read32s(sp);
        reg16[SP] += 4;
        return result;
    }
}

void pusha16()
{
    uint16_t temp = reg16[SP];

    // make sure we don't get a pagefault after having
    // pushed several registers already
    writable_or_pagefault(get_stack_pointer(-16), 16);

    push16(reg16[AX]);
    push16(reg16[CX]);
    push16(reg16[DX]);
    push16(reg16[BX]);
    push16(temp);
    push16(reg16[BP]);
    push16(reg16[SI]);
    push16(reg16[DI]);
}

void pusha32()
{
    int32_t temp = reg32s[ESP];

    writable_or_pagefault(get_stack_pointer(-32), 32);

    push32(reg32s[EAX]);
    push32(reg32s[ECX]);
    push32(reg32s[EDX]);
    push32(reg32s[EBX]);
    push32(temp);
    push32(reg32s[EBP]);
    push32(reg32s[ESI]);
    push32(reg32s[EDI]);
}

void setcc_reg(bool condition, int32_t r) {
    write_reg8(r, condition ? 1 : 0);
}

void setcc_mem(bool condition, int32_t addr) {
    safe_write8(addr, condition ? 1 : 0);
}

int32_t fpu_load_status_word();
void fpu_set_status_word(int32_t);
void fpu_store_m80(uint32_t, double_t);
double_t fpu_load_m80(uint32_t);

void fxsave(uint32_t addr)
{
    writable_or_pagefault(addr, 512);

    safe_write16(addr +  0, *fpu_control_word);
    safe_write16(addr +  2, fpu_load_status_word());
    safe_write8( addr +  4, ~*fpu_stack_empty & 0xFF);
    safe_write16(addr +  6, *fpu_opcode);
    safe_write32(addr +  8, *fpu_ip);
    safe_write16(addr + 12, *fpu_ip_selector);
    safe_write32(addr + 16, *fpu_dp);
    safe_write16(addr + 20, *fpu_dp_selector);

    safe_write32(addr + 24, *mxcsr);
    safe_write32(addr + 28, MXCSR_MASK);

    for(int32_t i = 0; i < 8; i++)
    {
        fpu_store_m80(addr + 32 + (i << 4), fpu_st[*fpu_stack_ptr + i & 7]);
    }

    // If the OSFXSR bit in control register CR4 is not set, the FXSAVE
    // instruction may not save these registers. This behavior is
    // implementation dependent.
    for(int32_t i = 0; i < 8; i++)
    {
        safe_write32(addr + 160 + (i << 4) +  0, reg_xmm[i].u32[0]);
        safe_write32(addr + 160 + (i << 4) +  4, reg_xmm[i].u32[1]);
        safe_write32(addr + 160 + (i << 4) +  8, reg_xmm[i].u32[2]);
        safe_write32(addr + 160 + (i << 4) + 12, reg_xmm[i].u32[3]);
    }
}

void fxrstor(uint32_t addr)
{
    translate_address_read(addr);
    translate_address_read(addr + 511);

    int32_t new_mxcsr = safe_read32s(addr + 24);

    if(new_mxcsr & ~MXCSR_MASK)
    {
        dbg_log("#gp Invalid mxcsr bits");
        trigger_gp(0);
    }

    *fpu_control_word = safe_read16(addr + 0);
    fpu_set_status_word(safe_read16(addr + 2));
    *fpu_stack_empty = ~safe_read8(addr + 4) & 0xFF;
    *fpu_opcode = safe_read16(addr + 6);
    *fpu_ip = safe_read32s(addr + 8);
    *fpu_ip = safe_read16(addr + 12);
    *fpu_dp = safe_read32s(addr + 16);
    *fpu_dp_selector = safe_read16(addr + 20);

    *mxcsr = new_mxcsr;

    for(int32_t i = 0; i < 8; i++)
    {
        fpu_st[*fpu_stack_ptr + i & 7] = fpu_load_m80(addr + 32 + (i << 4));
    }

    for(int32_t i = 0; i < 8; i++)
    {
        reg_xmm[i].u32[0] = safe_read32s(addr + 160 + (i << 4) +  0);
        reg_xmm[i].u32[1] = safe_read32s(addr + 160 + (i << 4) +  4);
        reg_xmm[i].u32[2] = safe_read32s(addr + 160 + (i << 4) +  8);
        reg_xmm[i].u32[3] = safe_read32s(addr + 160 + (i << 4) + 12);
    }
}

int32_t xchg8(int32_t data, int32_t r8)
{
    int32_t tmp = reg8[r8];
    reg8[r8] = data;
    return tmp;
}

int32_t xchg16(int32_t data, int32_t r16)
{
    int32_t tmp = reg16[r16];
    reg16[r16] = data;
    return tmp;
}

void xchg16r(int32_t r16)
{
    int32_t tmp = reg16[AX];
    reg16[AX] = reg16[r16];
    reg16[r16] = tmp;
}

int32_t xchg32(int32_t data, int32_t r32)
{
    int32_t tmp = reg32s[r32];
    reg32s[r32] = data;
    return tmp;
}

void xchg32r(int32_t r32)
{
    int32_t tmp = reg32s[EAX];
    reg32s[EAX] = reg32s[r32];
    reg32s[r32] = tmp;
}

void mov_r_m64(int32_t addr, int32_t r)
{
    // mov* m64, xmm
    task_switch_test_mmx();
    union reg64 data = read_xmm64s(r);
    safe_write64(addr, data.u64[0]);
}

void mov_r_r128(int32_t r1, int32_t r2)
{
    // mov* xmm, xmm
    task_switch_test_mmx();
    union reg128 data = read_xmm128s(r2);
    write_xmm_reg128(r1, data);
}

void mov_r_m128(int32_t addr, int32_t r)
{
    // mov* m128, xmm
    task_switch_test_mmx();
    union reg128 data = read_xmm128s(r);
    safe_write128(addr, data);
}

void mov_rm_r128(union reg128 source, int32_t r)
{
    // mov* xmm, xmm/m128
    task_switch_test_mmx();
    write_xmm_reg128(r, source);
}

void mov_m64_r128(int32_t addr, int32_t r)
{
    // movhp* xmm, m64
    task_switch_test_mmx();
    union reg64 data = safe_read64s(addr);
    union reg128 orig = read_xmm128s(r);
    write_xmm128(r, orig.u32[0], orig.u32[1], data.u32[0], data.u32[1]);
}

void mov_r128_m64(int32_t addr, int32_t r)
{
    // movhp* m64, xmm
    task_switch_test_mmx();
    union reg128 data = read_xmm128s(r);
    safe_write64(addr, data.u64[1]);
}

void psrlq_r128(int32_t r, uint32_t shift)
{
    // psrlq xmm, {shift}
    task_switch_test_mmx();

    if(shift == 0)
    {
        return;
    }

    union reg128 destination = read_xmm128s(r);
    union reg128 result = { { 0 } };

    if (shift <= 31)
    {
        result.u32[0] = destination.u32[0] >> shift | destination.u32[1] << (32 - shift);
        result.u32[1] = destination.u32[1] >> shift;

        result.u32[2] = destination.u32[2] >> shift | destination.u32[3] << (32 - shift);
        result.u32[3] = destination.u32[3] >> shift;
    }
    else if (shift <= 63)
    {
        result.u32[0] = destination.u32[1] >> shift;
        result.u32[2] = destination.u32[3] >> shift;
    }

    write_xmm_reg128(r, result);
}
