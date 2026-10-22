#include "cpu.h"
#include "core.h"
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <limits.h>

#include "bus.h"
#include "debug.h"

#define BIOS_START 0x1FC00000

enum HakuyaRegisterAlias {
	REG_ZERO = 0,  // Always zero
	REG_AT   = 1,  // Reserved
	REG_GP   = 28, // Global pointer
	REG_SP   = 29, // Stack pointer
	REG_FP   = 30, // Frame pointer
	REG_RA   = 31, // Return Address
};

enum COP0Register {
	REG_CONFIG  = 3,
	REG_BADADDR = 8,
	REG_SR      = 12,
	REG_CAUSE   = 13,
	REG_EPC     = 14,
	REG_ID      = 15,
};

enum ExceptionCode {
	EXCEPTION_INTERRUPT = 0,
	EXCEPTION_ADDRESS_LOAD_ERROR = 4,
	EXCEPTION_ADDRESS_STORE_ERROR = 5,
	EXCEPTION_BUS_FETCH_ERROR = 6,
	EXCEPTION_BUS_LOAD_ERROR = 7,
	EXCEPTION_SYSCALL = 8,
	EXCEPTION_BREAK = 9,
	EXCEPTION_RESERVED_INSTRUCTION = 10,
	EXCEPTION_COPROCESSOR_UNUSABLE = 11,
	EXCEPTION_OVERFLOW = 12,
};

struct InstructionTypeR {
	uint8_t opcode;
	uint8_t rs;
	uint8_t rt;
	uint8_t rd;
	uint8_t shamt;
	uint8_t funct;
};

struct InstructionTypeI {
	uint8_t  opcode;
	uint8_t  rs;
	uint8_t  rt;
	uint16_t immediate;
};

struct InstructionTypeJ {
	uint8_t  opcode;
	uint32_t target;
};

static inline void raise_exception(struct HakuyaCore *core, enum ExceptionCode code) {
        // The first 6 bits of the SR register represent a 3-deep and 2-bit wide stack.
	// The 2 bits represent "Kernel mode" and "Allow interrupts".
	uint32_t mode = (core->cpu.cop0_regs[REG_SR] << 2) & 0x3F;
	mode |= 0x02; // Turn on kernel mode and ignore interrupts
	core->cpu.cop0_regs[REG_SR] &= ~(uint32_t)0x3F;
	core->cpu.cop0_regs[REG_SR] |= mode;

	// Record the cause and the current PC
	core->cpu.cop0_regs[REG_CAUSE] &= ~(uint32_t)0x7Cu;
	core->cpu.cop0_regs[REG_CAUSE] |= (uint32_t)code << 2;
	core->cpu.cop0_regs[REG_EPC]   = core->cpu.pc;

	// If we're in a branch delay slot
	if (core->cpu.next_pc - core->cpu.pc != 4) {
		core->cpu.cop0_regs[REG_EPC]   -= 4;
		core->cpu.cop0_regs[REG_CAUSE] |= (1u << 31);
	} else {
		core->cpu.cop0_regs[REG_CAUSE] &= ~(1u << 31);
	}

	// Jump with no delay slot depending on the bev bit
	bool bev_bit = core->cpu.cop0_regs[REG_SR] & (1 << 22);
	core->cpu.next_pc = bev_bit ? 0xBFC00180 : 0x80000080;
	core->cpu.next_next_pc = core->cpu.next_pc + 4;
}

static inline void cpu_reg_set(struct HakuyaCore *core, uint32_t reg, uint32_t value) {
	if (reg == REG_ZERO) return;
	core->cpu.regs[reg] = value;

	if (reg == core->cpu.pending_load[0].reg) {
		core->cpu.pending_load[0] = (struct PendingLoad){ 0 };
	}
}

static inline void cpu_reg_set_pending(struct HakuyaCore *core, uint32_t reg, uint32_t value) {
	if (reg == REG_ZERO) return;

	if (reg == core->cpu.pending_load[0].reg) {
		core->cpu.pending_load[0] = (struct PendingLoad){ 0 };
	}
	core->cpu.pending_load[1].reg = reg;
	core->cpu.pending_load[1].value = value;
}

static inline void advance_pending_loads(struct HakuyaCore *core) {
	core->cpu.regs[core->cpu.pending_load[0].reg] = core->cpu.pending_load[0].value;
	core->cpu.pending_load[0] = core->cpu.pending_load[1];
	core->cpu.pending_load[1] = (struct PendingLoad){ 0 };
}

void cpu_init(struct HakuyaCore *core) {
	memset(&core->cpu, 0, sizeof(struct HakuyaCPU));
	core->cpu.pc = BIOS_START;
	core->cpu.next_pc = core->cpu.pc + 4;
	core->cpu.next_next_pc = core->cpu.next_pc + 4;
}

void cpu_print_regs(struct HakuyaCore *core) {
	printf("PC: %08X\n", core->cpu.pc);
	for (int i = 0; i < 32; i += 4) {
		printf("R%02d = %08X  R%02d = %08X  R%02d = %08X  R%02d = %08X\n",
		       i+0, core->cpu.regs[i+0], i+1, core->cpu.regs[i+1],
		       i+2, core->cpu.regs[i+2], i+3, core->cpu.regs[i+3]);
	}
	printf("\n");
}

static inline bool overflow_add(int32_t a, int32_t b, int32_t *result) {
	uint32_t ua = (uint32_t)a;
	uint32_t ub = (uint32_t)b;
	uint32_t uresult = ua + ub;
	*result = (int32_t)uresult;
	// If the signs of a and b are the same and the result has a different
	// sign from a, then a signed overflow as occured.
	return ((~(ua ^ ub) & (ua ^ uresult)) >> 31);
}

static inline bool overflow_sub(int32_t a, int32_t b, int32_t *result) {
	uint32_t ua = (uint32_t)a;
	uint32_t ub = (uint32_t)b;
	uint32_t uresult = ua - ub;
	*result = (int32_t)uresult;
	// If the signs of a and b are different and the result has a different
	// sign from a, then a signed overflow as occured.
	return (((ua ^ ub) & (ua ^ uresult)) >> 31);
}

static inline void branch(struct HakuyaCore *core, int32_t offset) {
	int32_t target = (int32_t)core->cpu.pc + 4 + offset * 4;
	core->cpu.next_next_pc = (uint32_t)target;
}

static inline void op_bltz(struct HakuyaCore *core, struct InstructionTypeI ins) {
	int32_t imm = (int32_t)(int16_t)ins.immediate;
	if ((int32_t)core->cpu.regs[ins.rs] < 0)
		branch(core, imm);
}

static inline void op_bgez(struct HakuyaCore *core, struct InstructionTypeI ins) {
	int32_t imm = (int32_t)(int16_t)ins.immediate;
	if ((int32_t)core->cpu.regs[ins.rs] >= 0)
		branch(core, imm);
}

static inline void op_bltzal(struct HakuyaCore *core, struct InstructionTypeI ins) {
	int32_t imm = (int32_t)(int16_t)ins.immediate;
	if ((int32_t)core->cpu.regs[ins.rs] < 0)
		branch(core, imm);
	cpu_reg_set(core, REG_RA, core->cpu.pc + 8);
}

static inline void op_bgezal(struct HakuyaCore *core, struct InstructionTypeI ins) {
	int32_t imm = (int32_t)(int16_t)ins.immediate;
	if ((int32_t)core->cpu.regs[ins.rs] >= 0)
		branch(core, imm);
	cpu_reg_set(core, REG_RA, core->cpu.pc + 8);
}

static inline void op_beq(struct HakuyaCore *core, struct InstructionTypeI ins) {
	int32_t imm = (int32_t)(int16_t)ins.immediate;
	if (core->cpu.regs[ins.rs] == core->cpu.regs[ins.rt])
		branch(core, imm);
}

static inline void op_bne(struct HakuyaCore *core, struct InstructionTypeI ins) {
	int32_t imm = (int32_t)(int16_t)ins.immediate;
	if (core->cpu.regs[ins.rs] != core->cpu.regs[ins.rt])
		branch(core, imm);
}

static inline void op_blez(struct HakuyaCore *core, struct InstructionTypeI ins) {
	int32_t imm = (int32_t)(int16_t)ins.immediate;
	if ((int32_t)core->cpu.regs[ins.rs] <= 0)
		branch(core, imm);
}

static inline void op_bgtz(struct HakuyaCore *core, struct InstructionTypeI ins) {
	int32_t imm = (int32_t)(int16_t)ins.immediate;
	if ((int32_t)core->cpu.regs[ins.rs] > 0)
		branch(core, imm);
}

static inline void op_addi(struct HakuyaCore *core, struct InstructionTypeI ins) {
	int32_t a = (int32_t)(int16_t)ins.immediate;
	int32_t b = (int32_t)core->cpu.regs[ins.rs];
	int32_t result;
	if (overflow_add(a, b, &result)) {
		raise_exception(core, EXCEPTION_OVERFLOW);
		return;
	}
	cpu_reg_set(core, ins.rt, (uint32_t)result);
}

static inline void op_addiu(struct HakuyaCore *core, struct InstructionTypeI ins) {
	cpu_reg_set(core, ins.rt, core->cpu.regs[ins.rs] + (uint32_t)(int32_t)(int16_t)ins.immediate);
}

static inline void op_slti(struct HakuyaCore *core, struct InstructionTypeI ins) {
	int32_t imm = (int32_t)(int16_t)ins.immediate;
	cpu_reg_set(core, ins.rt, (int32_t)core->cpu.regs[ins.rs] < imm);
}

static inline void op_sltiu(struct HakuyaCore *core, struct InstructionTypeI ins) {
	uint32_t imm = (uint32_t)(int32_t)(int16_t)ins.immediate;
	cpu_reg_set(core, ins.rt, core->cpu.regs[ins.rs] < imm);
}

static inline void op_andi(struct HakuyaCore *core, struct InstructionTypeI ins) {
	cpu_reg_set(core, ins.rt, core->cpu.regs[ins.rs] & (uint32_t)ins.immediate);
}

static inline void op_ori(struct HakuyaCore *core, struct InstructionTypeI ins) {
	cpu_reg_set(core, ins.rt, core->cpu.regs[ins.rs] | (uint32_t)ins.immediate);
}

static inline void op_xori(struct HakuyaCore *core, struct InstructionTypeI ins) {
	cpu_reg_set(core, ins.rt, core->cpu.regs[ins.rs] ^ (uint32_t)ins.immediate);
}

static inline void op_lui(struct HakuyaCore *core, struct InstructionTypeI ins) {
	cpu_reg_set(core, ins.rt, (uint32_t)ins.immediate << 16);
}

static inline void op_lb(struct HakuyaCore *core, struct InstructionTypeI ins) {
	uint32_t address = core->cpu.regs[ins.rs] + (uint32_t)(int32_t)(int16_t)ins.immediate;
	if ((core->cpu.cop0_regs[REG_SR] & 0x00010000) != 0) {
		/* fprintf(stderr, "[WARNING] Load at %08X was ignored since cache is isolated\n", address); */
		return;
	}
	cpu_reg_set_pending(core, ins.rt, (uint32_t)(int32_t)(int8_t)bus_read8(core, address));
}

static inline void op_lh(struct HakuyaCore *core, struct InstructionTypeI ins) {
	uint32_t address = core->cpu.regs[ins.rs] + (uint32_t)(int32_t)(int16_t)ins.immediate;
	if (address % 2 != 0) {
		raise_exception(core, EXCEPTION_ADDRESS_LOAD_ERROR);
		return;
	}
	if ((core->cpu.cop0_regs[REG_SR] & 0x00010000) != 0) {
		/* fprintf(stderr, "[WARNING] Store at %08X was ignored since cache is isolated\n", address); */
		return;
	}
	cpu_reg_set_pending(core, ins.rt, (uint32_t)(int32_t)(int16_t)bus_read16(core, address));
}

static inline void op_lwl(struct HakuyaCore *core, struct InstructionTypeI ins) {
	uint32_t address = core->cpu.regs[ins.rs] + (uint32_t)(int32_t)(int16_t)ins.immediate;
	uint32_t aligned_address = address & (~(uint32_t)0x03);
	uint32_t value = bus_read32(core, aligned_address);
	uint32_t result;
	if (core->cpu.pending_load[0].reg == ins.rt) {
		result = core->cpu.pending_load[0].value;
		core->cpu.pending_load[0] = (struct PendingLoad){ 0 };
	} else {
		result = core->cpu.regs[ins.rt];
	}

	switch (address % 4) {
	case 0: result &= 0x00FFFFFF; result |= (value << 24); break;
	case 1: result &= 0x0000FFFF; result |= (value << 16); break;
	case 2: result &= 0x000000FF; result |= (value <<  8); break;
	case 3: result &= 0x00000000; result |= (value <<  0); break;
	}

	cpu_reg_set_pending(core, ins.rt, result);
}

static inline void op_lwr(struct HakuyaCore *core, struct InstructionTypeI ins) {
	uint32_t address = core->cpu.regs[ins.rs] + (uint32_t)(int32_t)(int16_t)ins.immediate;
	uint32_t aligned_address = address & (~(uint32_t)0x03);
	uint32_t value = bus_read32(core, aligned_address);
	uint32_t result;
	if (core->cpu.pending_load[0].reg == ins.rt) {
		result = core->cpu.pending_load[0].value;
		core->cpu.pending_load[0] = (struct PendingLoad){ 0 };
	} else {
		result = core->cpu.regs[ins.rt];
	}

	switch (address % 4) {
	case 0: result &= 0x00000000; result |= (value >>  0); break;
	case 1: result &= 0xFF000000; result |= (value >>  8); break;
	case 2: result &= 0xFFFF0000; result |= (value >> 16); break;
	case 3: result &= 0xFFFFFF00; result |= (value >> 24); break;
	}

	cpu_reg_set_pending(core, ins.rt, result);
}

static inline void op_swl(struct HakuyaCore *core, struct InstructionTypeI ins) {
	uint32_t address = core->cpu.regs[ins.rs] + (uint32_t)(int32_t)(int16_t)ins.immediate;
	uint32_t aligned_address = address & (~(uint32_t)0x03);
	uint32_t value  = core->cpu.regs[ins.rt];
	uint32_t result = bus_read32(core, aligned_address);

	switch (address % 4) {
	case 0: result &= 0xFFFFFF00; result |= (value >> 24); break;
	case 1: result &= 0xFFFF0000; result |= (value >> 16); break;
	case 2: result &= 0xFF000000; result |= (value >>  8); break;
	case 3: result &= 0x00000000; result |= (value >>  0); break;
	}

	bus_write32(core, aligned_address, result);
}

static inline void op_swr(struct HakuyaCore *core, struct InstructionTypeI ins) {
	uint32_t address = core->cpu.regs[ins.rs] + (uint32_t)(int32_t)(int16_t)ins.immediate;
	uint32_t aligned_address = address & (~(uint32_t)0x03);
	uint32_t value  = core->cpu.regs[ins.rt];
	uint32_t result = bus_read32(core, aligned_address);

	switch (address % 4) {
	case 0: result &= 0x00000000; result |= (value <<  0); break;
	case 1: result &= 0x000000FF; result |= (value <<  8); break;
	case 2: result &= 0x0000FFFF; result |= (value << 16); break;
	case 3: result &= 0x00FFFFFF; result |= (value << 24); break;
	}

	bus_write32(core, aligned_address, result);
}

static inline void op_lw(struct HakuyaCore *core, struct InstructionTypeI ins) {
	uint32_t address = core->cpu.regs[ins.rs] + (uint32_t)(int32_t)(int16_t)ins.immediate;
	if (address % 4 != 0) {
		raise_exception(core, EXCEPTION_ADDRESS_LOAD_ERROR);
		return;
	}
	if ((core->cpu.cop0_regs[REG_SR] & 0x00010000) != 0) {
		/* fprintf(stderr, "[WARNING] Load at %08X was ignored since cache is isolated\n", address); */
		return;
	}
	cpu_reg_set_pending(core, ins.rt, bus_read32(core, address));
}

static inline void op_lbu(struct HakuyaCore *core, struct InstructionTypeI ins) {
	uint32_t address = core->cpu.regs[ins.rs] + (uint32_t)(int32_t)(int16_t)ins.immediate;
	if ((core->cpu.cop0_regs[REG_SR] & 0x00010000) != 0) {
		/* fprintf(stderr, "[WARNING] Load at %08X was ignored since cache is isolated\n", address); */
		return;
	}
	cpu_reg_set_pending(core, ins.rt, (uint32_t)bus_read8(core, address));
}

static inline void op_lhu(struct HakuyaCore *core, struct InstructionTypeI ins) {
	uint32_t address = core->cpu.regs[ins.rs] + (uint32_t)(int32_t)(int16_t)ins.immediate;
	if (address % 2 != 0) {
		raise_exception(core, EXCEPTION_ADDRESS_LOAD_ERROR);
		return;
	}
	if ((core->cpu.cop0_regs[REG_SR] & 0x00010000) != 0) {
		/* fprintf(stderr, "[WARNING] Load at %08X was ignored since cache is isolated\n", address); */
		return;
	}
	cpu_reg_set_pending(core, ins.rt, (uint32_t)bus_read16(core, address));
}

static inline void op_sb(struct HakuyaCore *core, struct InstructionTypeI ins) {
	uint32_t address = core->cpu.regs[ins.rs] + (uint32_t)(int32_t)(int16_t)ins.immediate;
	if ((core->cpu.cop0_regs[REG_SR] & 0x00010000) != 0) {
		/* fprintf(stderr, "[WARNING] Store at %08X was ignored since cache is isolated\n", address); */
		return;
	}
	bus_write8(core, address, core->cpu.regs[ins.rt] & 0xFF);
}

static inline void op_sh(struct HakuyaCore *core, struct InstructionTypeI ins) {
	uint32_t address = core->cpu.regs[ins.rs] + (uint32_t)(int32_t)(int16_t)ins.immediate;
	if (address % 2 != 0) {
		raise_exception(core, EXCEPTION_ADDRESS_STORE_ERROR);
		return;
	}
	if ((core->cpu.cop0_regs[REG_SR] & 0x00010000) != 0) {
		/* fprintf(stderr, "[WARNING] Store at %08X was ignored since cache is isolated\n", address); */
		return;
	}
	bus_write16(core, address, core->cpu.regs[ins.rt] & 0xFFFF);
}

static inline void op_sw(struct HakuyaCore *core, struct InstructionTypeI ins) {
	uint32_t address = core->cpu.regs[ins.rs] + (uint32_t)(int32_t)(int16_t)ins.immediate;
	if (address % 4 != 0) {
		raise_exception(core, EXCEPTION_ADDRESS_STORE_ERROR);
		return;
	}
	if ((core->cpu.cop0_regs[REG_SR] & 0x00010000) != 0) {
		/* fprintf(stderr, "[WARNING] Store at %08X was ignored since cache is isolated\n", address); */
		return;
	}
	bus_write32(core, address, core->cpu.regs[ins.rt]);
}

static inline void op_lwc0(struct HakuyaCore *core, struct InstructionTypeI ins) {
	(void)ins;
	raise_exception(core, EXCEPTION_COPROCESSOR_UNUSABLE);
}

static inline void op_lwc1(struct HakuyaCore *core, struct InstructionTypeI ins) {
	(void)ins;
	raise_exception(core, EXCEPTION_COPROCESSOR_UNUSABLE);
}

static inline void op_lwc2(struct HakuyaCore *core, struct InstructionTypeI ins) {
	(void)core;
	(void)ins;
	PANIC("Error: %s", "Unimplemented COP2 load word instruction");
}

static inline void op_lwc3(struct HakuyaCore *core, struct InstructionTypeI ins) {
	(void)ins;
	raise_exception(core, EXCEPTION_COPROCESSOR_UNUSABLE);
}

static inline void op_swc0(struct HakuyaCore *core, struct InstructionTypeI ins) {
	(void)ins;
	raise_exception(core, EXCEPTION_COPROCESSOR_UNUSABLE);
}

static inline void op_swc1(struct HakuyaCore *core, struct InstructionTypeI ins) {
	(void)ins;
	raise_exception(core, EXCEPTION_COPROCESSOR_UNUSABLE);
}

static inline void op_swc2(struct HakuyaCore *core, struct InstructionTypeI ins) {
	(void)core;
	(void)ins;
	PANIC("Error: %s", "Unimplemented COP2 store word instruction");
}

static inline void op_swc3(struct HakuyaCore *core, struct InstructionTypeI ins) {
	(void)ins;
	raise_exception(core, EXCEPTION_COPROCESSOR_UNUSABLE);
}

static inline void op_sll(struct HakuyaCore *core, struct InstructionTypeR ins) {
	cpu_reg_set(core, ins.rd, core->cpu.regs[ins.rt] << ins.shamt);
}

static inline void op_srl(struct HakuyaCore *core, struct InstructionTypeR ins) {
	cpu_reg_set(core, ins.rd, core->cpu.regs[ins.rt] >> ins.shamt);
}

static inline void op_sra(struct HakuyaCore *core, struct InstructionTypeR ins) {
	// Get guaranteed arithmetic right shift behavior. On gcc -O2, this
	// converts into a single sra instruction as desired.
	int32_t signed_num = (int32_t)core->cpu.regs[ins.rt];
	if (signed_num < 0)
		signed_num = ~(~signed_num >> ins.shamt);
	else
		signed_num = signed_num >> ins.shamt;
	cpu_reg_set(core, ins.rd, (uint32_t)signed_num);
}

static inline void op_sllv(struct HakuyaCore *core, struct InstructionTypeR ins) {
	uint32_t shift_amount = core->cpu.regs[ins.rs] & 0x1F;
	cpu_reg_set(core, ins.rd, core->cpu.regs[ins.rt] << shift_amount);
}

static inline void op_srlv(struct HakuyaCore *core, struct InstructionTypeR ins) {
	uint32_t shift_amount = core->cpu.regs[ins.rs] & 0x1F;
	cpu_reg_set(core, ins.rd, core->cpu.regs[ins.rt] >> shift_amount);
}

static inline void op_srav(struct HakuyaCore *core, struct InstructionTypeR ins) {
	uint32_t shift_amount = core->cpu.regs[ins.rs] & 0x1F;
	// Get guaranteed arithmetic right shift behavior. On gcc -O2, this
	// converts into a single sra instruction as desired.
	int32_t signed_num = (int32_t)core->cpu.regs[ins.rt];
	if (signed_num < 0)
		signed_num = ~(~signed_num >> shift_amount);
	else
		signed_num = signed_num >> shift_amount;
	cpu_reg_set(core, ins.rd, (uint32_t)signed_num);
}

static inline void op_jr(struct HakuyaCore *core, struct InstructionTypeR ins) {
	if (core->cpu.regs[ins.rs] % 4 != 0) {
		raise_exception(core, EXCEPTION_ADDRESS_LOAD_ERROR);
		return;
	}
	core->cpu.next_next_pc = core->cpu.regs[ins.rs];
}

static inline void op_jalr(struct HakuyaCore *core, struct InstructionTypeR ins) {
	uint32_t jump_target = core->cpu.regs[ins.rs];
	cpu_reg_set(core, ins.rd, core->cpu.pc + 8);
	if (jump_target % 4 != 0)
	{
		raise_exception(core, EXCEPTION_ADDRESS_LOAD_ERROR);
		return;
	}
	core->cpu.next_next_pc = jump_target;
}

static inline void op_syscall(struct HakuyaCore *core, struct InstructionTypeR ins) {
	(void)ins;
	raise_exception(core, EXCEPTION_SYSCALL);
}

static inline void op_break(struct HakuyaCore *core, struct InstructionTypeR ins) {
	(void)ins;
	raise_exception(core, EXCEPTION_BREAK);
}

static inline void op_mfhi(struct HakuyaCore *core, struct InstructionTypeR ins) {
	cpu_reg_set(core, ins.rd, core->cpu.hi);
}

static inline void op_mthi(struct HakuyaCore *core, struct InstructionTypeR ins) {
	core->cpu.hi = core->cpu.regs[ins.rs];
}

static inline void op_mflo(struct HakuyaCore *core, struct InstructionTypeR ins) {
	cpu_reg_set(core, ins.rd, core->cpu.lo);
}

static inline void op_mtlo(struct HakuyaCore *core, struct InstructionTypeR ins) {
	core->cpu.lo = core->cpu.regs[ins.rs];
}

static inline void op_mult(struct HakuyaCore *core, struct InstructionTypeR ins) {
	int64_t a = (int64_t)(int32_t)core->cpu.regs[ins.rs];
	int64_t b = (int64_t)(int32_t)core->cpu.regs[ins.rt];
	uint64_t result = (uint64_t)(a * b);
	core->cpu.lo = (result & 0xFFFFFFFF);
	core->cpu.hi = (result >> 32);
}

static inline void op_multu(struct HakuyaCore *core, struct InstructionTypeR ins) {
	uint64_t a = (uint64_t)core->cpu.regs[ins.rs];
	uint64_t b = (uint64_t)core->cpu.regs[ins.rt];
	uint64_t result = a * b;
	core->cpu.lo = (result & 0xFFFFFFFF);
	core->cpu.hi = (result >> 32);
}

static inline void op_div(struct HakuyaCore *core, struct InstructionTypeR ins) {
	int32_t num = (int32_t)core->cpu.regs[ins.rs];
	int32_t dem = (int32_t)core->cpu.regs[ins.rt];
	if (dem == 0) {
		core->cpu.hi = (uint32_t)0;
		core->cpu.lo = (uint32_t)0;
		core->cpu.hi = (uint32_t)num;
		core->cpu.lo = (num < 0) ? 0x00000001u : 0xFFFFFFFFu;
	} else if (num == INT32_MIN && dem == -1) {
		core->cpu.hi = 0x00000000u;
		core->cpu.lo = 0x80000000u;
	} else {
		core->cpu.lo = (uint32_t)(num / dem);
		core->cpu.hi = (uint32_t)(num % dem);
	}
}

static inline void op_divu(struct HakuyaCore *core, struct InstructionTypeR ins) {
	uint32_t num = core->cpu.regs[ins.rs];
	uint32_t dem = core->cpu.regs[ins.rt];
	if (dem == 0) {
		core->cpu.hi = num;
		core->cpu.lo = 0xFFFFFFFFu;
	} else {
		core->cpu.lo = (uint32_t)(num / dem);
		core->cpu.hi = (uint32_t)(num % dem);
	}
}

static inline void op_add(struct HakuyaCore *core, struct InstructionTypeR ins) {
	int32_t a = (int32_t)core->cpu.regs[ins.rs];
	int32_t b = (int32_t)core->cpu.regs[ins.rt];
	int32_t result;
	if (overflow_add(a, b, &result)) {
		raise_exception(core, EXCEPTION_OVERFLOW);
		return;
	}
	cpu_reg_set(core, ins.rd, (uint32_t)result);
}

static inline void op_addu(struct HakuyaCore *core, struct InstructionTypeR ins) {
	cpu_reg_set(core, ins.rd, core->cpu.regs[ins.rs] + core->cpu.regs[ins.rt]);
}

static inline void op_sub(struct HakuyaCore *core, struct InstructionTypeR ins) {
	int32_t a = (int32_t)core->cpu.regs[ins.rs];
	int32_t b = (int32_t)core->cpu.regs[ins.rt];
	int32_t result;
	if (overflow_sub(a, b, &result)) {
		raise_exception(core, EXCEPTION_OVERFLOW);
		return;
	}
	cpu_reg_set(core, ins.rd, (uint32_t)result);
}

static inline void op_subu(struct HakuyaCore *core, struct InstructionTypeR ins) {
	cpu_reg_set(core, ins.rd, core->cpu.regs[ins.rs] - core->cpu.regs[ins.rt]);
}

static inline void op_and(struct HakuyaCore *core, struct InstructionTypeR ins) {
	cpu_reg_set(core, ins.rd, core->cpu.regs[ins.rs] & core->cpu.regs[ins.rt]);
}

static inline void op_or(struct HakuyaCore *core, struct InstructionTypeR ins) {
	cpu_reg_set(core, ins.rd, core->cpu.regs[ins.rs] | core->cpu.regs[ins.rt]);
}

static inline void op_xor(struct HakuyaCore *core, struct InstructionTypeR ins) {
	cpu_reg_set(core, ins.rd, core->cpu.regs[ins.rs] ^ core->cpu.regs[ins.rt]);
}

static inline void op_nor(struct HakuyaCore *core, struct InstructionTypeR ins) {
	cpu_reg_set(core, ins.rd, ~(core->cpu.regs[ins.rs] | core->cpu.regs[ins.rt]));
}

static inline void op_slt(struct HakuyaCore *core, struct InstructionTypeR ins) {
	cpu_reg_set(core, ins.rd, (int32_t)core->cpu.regs[ins.rs] < (int32_t)core->cpu.regs[ins.rt]);
}

static inline void op_sltu(struct HakuyaCore *core, struct InstructionTypeR ins) {
	cpu_reg_set(core, ins.rd, core->cpu.regs[ins.rs] < core->cpu.regs[ins.rt]);
}

static inline void op_j(struct HakuyaCore *core, struct InstructionTypeJ ins) {
	core->cpu.next_next_pc = (core->cpu.next_pc & 0xF0000000) | (ins.target << 2);
}

static inline void op_jal(struct HakuyaCore *core, struct InstructionTypeJ ins) {
	cpu_reg_set(core, REG_RA, core->cpu.pc + 8);
	core->cpu.next_next_pc = (core->cpu.next_pc & 0xF0000000) | (ins.target << 2);
}

static inline uint32_t bit_slice(uint32_t num, int hi, int lo) {
	int width = hi - lo + 1;
	assert(width < 32 && width > 0);
	uint32_t mask = (1u << width) - 1;
	return (num >> lo) & mask;
}

static void unimplemented(uint32_t instruction, const char *name) {
	PANIC("The instruction 0x%08X (op_%s) is unimplemented", instruction, name);
}

static void execute_r_instruction(struct HakuyaCore *core, uint32_t instruction) {
	struct InstructionTypeR ins = {
		.opcode = (uint8_t)bit_slice(instruction, 31, 26),
		.rs     = (uint8_t)bit_slice(instruction, 25, 21),
		.rt     = (uint8_t)bit_slice(instruction, 20, 16),
		.rd     = (uint8_t)bit_slice(instruction, 15, 11),
		.shamt  = (uint8_t)bit_slice(instruction, 10,  6),
		.funct  = (uint8_t)bit_slice(instruction,  5,  0),
	};

	switch (ins.funct) {
	case 0x00: op_sll (core, ins); break;
	// 0x01 is unused
	case 0x02: op_srl (core, ins); break;
	case 0x03: op_sra (core, ins); break;
	case 0x04: op_sllv(core, ins); break;
	// 0x05 is unused
	case 0x06: op_srlv(core, ins); break;
	case 0x07: op_srav(core, ins); break;
	case 0x08: op_jr  (core, ins); break;
	case 0x09: op_jalr(core, ins); break;
	// 0x0A and 0x0B are unused
	case 0x0C: op_syscall(core, ins); break;
	case 0x0D: op_break  (core, ins); break;
	// 0x0E and 0x0F are unused
	case 0x10: op_mfhi(core, ins); break;
	case 0x11: op_mthi(core, ins); break;
	case 0x12: op_mflo(core, ins); break;
	case 0x13: op_mtlo(core, ins); break;
	// 0x14 ~ 0x17 are unused
	case 0x18: op_mult (core, ins); break;
	case 0x19: op_multu(core, ins); break;
	case 0x1A: op_div (core, ins); break;
	case 0x1B: op_divu(core, ins); break;
	// 0x1C ~ 0x1F are unused
	case 0x20: op_add (core, ins); break;
	case 0x21: op_addu(core, ins); break;
	case 0x22: op_sub (core, ins); break;
	case 0x23: op_subu(core, ins); break;
	case 0x24: op_and (core, ins); break;
	case 0x25: op_or  (core, ins); break;
	case 0x26: op_xor (core, ins); break;
	case 0x27: op_nor (core, ins); break;
	// 0x28 and 0x29 are unused
	case 0x2A: op_slt (core, ins); break;
	case 0x2B: op_sltu(core, ins); break;
	// 0x2C ~ 0x3F are unused
	default: PANIC("Unknown R instruction (instruction: 0x%08X, funct: 0x%02X)",
		       instruction, ins.funct);
	}
}

static void execute_i_instruction(struct HakuyaCore *core, uint32_t instruction) {
	struct InstructionTypeI ins = {
		.opcode    = (uint8_t)bit_slice(instruction, 31, 26),
		.rs        = (uint8_t)bit_slice(instruction, 25, 21),
		.rt        = (uint8_t)bit_slice(instruction, 20, 16),
		.immediate = (uint16_t)bit_slice(instruction, 15,  0),
	};

	switch (ins.opcode) {
	case 0x01: // The bcond opcodes
		switch (bit_slice(instruction, 20, 16)) {
		case 0x00: op_bltz  (core, ins); break;
		case 0x01: op_bgez  (core, ins); break;
		case 0x10: op_bltzal(core, ins); break;
		case 0x11: op_bgezal(core, ins); break;
		default:
			if (instruction & (1u << 16))
				op_bgez(core, ins);
			else
				op_bltz(core, ins);
			break;
		}
		break;
	// 0x02 and 0x03 are jump codes here
	case 0x04: op_beq  (core, ins); break;
	case 0x05: op_bne  (core, ins); break;
	case 0x06: op_blez (core, ins); break;
	case 0x07: op_bgtz (core, ins); break;
	case 0x08: op_addi (core, ins); break;
	case 0x09: op_addiu(core, ins); break;
	case 0x0A: op_slti (core, ins); break;
	case 0x0B: op_sltiu(core, ins); break;
	case 0x0C: op_andi (core, ins); break;
	case 0x0D: op_ori  (core, ins); break;
	case 0x0E: op_xori (core, ins); break;
	case 0x0F: op_lui  (core, ins); break;
	// 0x10, 0x11, 0x12, and 0x13 are coprocessor codes
	// 0x14 ~ 0x1F are unused
	case 0x20: op_lb   (core, ins); break;
	case 0x21: op_lh   (core, ins); break;
	case 0x22: op_lwl  (core, ins); break;
	case 0x23: op_lw   (core, ins); break;
	case 0x24: op_lbu  (core, ins); break;
	case 0x25: op_lhu  (core, ins); break;
	case 0x26: op_lwr  (core, ins); break;
	// 0x27 is unused
	case 0x28: op_sb   (core, ins); break;
	case 0x29: op_sh   (core, ins); break;
	case 0x2A: op_swl  (core, ins); break;
	case 0x2B: op_sw   (core, ins); break;
	// 0x2C and 0x2D are unused
	case 0x2E: op_swr  (core, ins); break;
	// 0x2F is unused
	case 0x30: op_lwc0(core, ins); break;
	case 0x31: op_lwc1(core, ins); break;
	case 0x32: op_lwc2(core, ins); break;
	case 0x33: op_lwc3(core, ins); break;
	// 0x34 ~ 0x37 are unused
	case 0x38: op_swc0(core, ins); break;
	case 0x39: op_swc1(core, ins); break;
	case 0x3A: op_swc2(core, ins); break;
	case 0x3B: op_swc3(core, ins); break;
	// 0x3C ~ 0x3F are unused
	default: PANIC("Unknown I instruction (instruction: 0x%08X, opcode: 0x%02X)",
		       instruction, ins.opcode);
	}
}

static void execute_j_instruction(struct HakuyaCore *core, uint32_t instruction) {
	struct InstructionTypeJ ins = {
		.opcode = (uint8_t)bit_slice(instruction, 31, 26),
		.target = (uint32_t)bit_slice(instruction, 25,  0),
	};

	switch (ins.opcode) {
	case 0x02: op_j  (core, ins); break;
	case 0x03: op_jal(core, ins); break;
	default: PANIC("Unknown J instruction (instruction: 0x%08X, opcode: 0x%02X)",
		       instruction, ins.opcode);
	}
}

static void op_mtc0(struct HakuyaCore *core, uint32_t instruction) {
	uint32_t rt = bit_slice(instruction, 20, 16);
	uint32_t rd = bit_slice(instruction, 15, 11);
	core->cpu.cop0_regs[rd] = core->cpu.regs[rt];
}

static void op_mfc0(struct HakuyaCore *core, uint32_t instruction) {
	uint32_t rt = bit_slice(instruction, 20, 16);
	uint32_t rd = bit_slice(instruction, 15, 11);
	cpu_reg_set_pending(core, rt, core->cpu.cop0_regs[rd]);
}

static void op_rfe(struct HakuyaCore *core, uint32_t instruction) {
	(void)instruction;
	uint32_t mode = core->cpu.cop0_regs[REG_SR] & 0x3F;
	core->cpu.cop0_regs[REG_SR] &= ~(uint32_t)0x0F;
	core->cpu.cop0_regs[REG_SR] |= (mode >> 2);
}

static void execute_cop0_instruction(struct HakuyaCore *core, uint32_t instruction) {
	uint32_t cop_opcode = bit_slice(instruction, 25, 21);

	switch (cop_opcode) {
	case 0x00: op_mfc0(core, instruction); break;
	case 0x02: unimplemented(instruction, "cf0"); break;
	case 0x04: op_mtc0(core, instruction); break;
	case 0x06: unimplemented(instruction, "ct0"); break;
	case 0x08: unimplemented(instruction, "bc0"); break;
	case 0x10: // COP0 special instructions
		switch (bit_slice(instruction, 5, 0)) {
		case 0x01: unimplemented(instruction, "tlbr"); break;
		case 0x02: unimplemented(instruction, "tlbwi"); break;
		case 0x06: unimplemented(instruction, "tlbwr"); break;
		case 0x08: unimplemented(instruction, "tlbp"); break;
		case 0x10: op_rfe(core, instruction); break;
		default: PANIC("Unknown COP0 sub instruction: 0x%08X", instruction);
		}
		break;
	default: PANIC("Unknown COP0 instruction: 0x%08X", instruction);
	}
}

static void execute_cop2_instruction(struct HakuyaCore *core, uint32_t instruction) {
	(void)core;
	PANIC("COP2 is unimplemented. (Instruction: %08X)", instruction);
}

static void execute_instruction(struct HakuyaCore *core, uint32_t instruction) {
	uint32_t opcode = bit_slice(instruction, 31, 26);
	switch (opcode) {
	case 0x00: execute_r_instruction(core, instruction);    break;
	case 0x02:
	case 0x03: execute_j_instruction(core, instruction);    break;
	case 0x10: execute_cop0_instruction(core, instruction); break;
	case 0x11: raise_exception(core, EXCEPTION_COPROCESSOR_UNUSABLE); break;
	case 0x12: execute_cop2_instruction(core, instruction); break;
	case 0x13: raise_exception(core, EXCEPTION_COPROCESSOR_UNUSABLE); break;
	default:   execute_i_instruction(core, instruction);    break;
	}
}

static void print_tty_output(struct HakuyaCore *core) {
	uint32_t pc = core->cpu.pc & 0x1FFFFFFF;
	if (pc == 0xA0 && core->cpu.regs[9] == 0x3C) {
		putc((char)core->cpu.regs[4], stdout);
	} else if (pc == 0xB0 && core->cpu.regs[9] == 0x3D) {
		putc((char)core->cpu.regs[4], stdout);
	}
}

static void check_interrupts(struct HakuyaCore *core) {
	uint32_t sr    = core->cpu.cop0_regs[REG_SR];
	uint32_t cause = core->cpu.cop0_regs[REG_CAUSE];

	// Set bit 10 of CAUSE if there's a hardware interrupt
	cause &= ~((uint32_t)1u << 10);
	cause |= ((uint32_t)bus_interrupt_is_pending(core) << 10);
	core->cpu.cop0_regs[REG_CAUSE] = cause;

	bool master_interrupts_enabled = sr & 0x01u;
	if (master_interrupts_enabled && (sr & cause & 0x700)) {
		raise_exception(core, EXCEPTION_INTERRUPT);
		// Advance the pc so that it's now pointing at the exception vector
		core->cpu.pc = core->cpu.next_pc;
		core->cpu.next_pc = core->cpu.next_next_pc;
		core->cpu.next_next_pc = core->cpu.next_pc + 4;
	}
}

void cpu_run_next_instruction(struct HakuyaCore *core) {
	check_interrupts(core);
	advance_pending_loads(core);
	uint32_t instruction = bus_read32(core, core->cpu.pc);
	execute_instruction(core, instruction);
	print_tty_output(core);

	core->cpu.pc = core->cpu.next_pc;
	core->cpu.next_pc = core->cpu.next_next_pc;
	core->cpu.next_next_pc = core->cpu.next_pc + 4;
}
