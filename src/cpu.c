#include "cpu.h"
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

static inline void raise_exception(struct HakuyaCPU *cpu, enum ExceptionCode code) {
        // The first 6 bits of the SR register represent a 3-deep and 2-bit wide stack.
	// The 2 bits represent "Kernel mode" and "Allow interrupts".
	uint32_t mode = (cpu->cop0_regs[12] << 2) & 0x3F;
	mode |= 0x02; // Turn on kernel mode and ignore interrupts
	cpu->cop0_regs[REG_SR] &= ~(uint32_t)0x3F;
	cpu->cop0_regs[REG_SR] |= mode;

	// Record the cause and the current PC
	cpu->cop0_regs[REG_CAUSE] = (uint32_t)code << 2;
	cpu->cop0_regs[REG_EPC]   = cpu->pc;

	// If we're in a branch delay slot
	if (cpu->next_pc - cpu->pc != 4) {
		cpu->cop0_regs[REG_EPC]   -= 4;
		cpu->cop0_regs[REG_CAUSE] |= (1u << 31);
	} else {
		cpu->cop0_regs[REG_CAUSE] &= ~(1u << 31);
	}

	// Jump with no delay slot depending on the bev bit
	bool bev_bit = cpu->cop0_regs[REG_SR] & (1 << 22);
	cpu->next_pc = bev_bit ? 0xBFC00180 : 0x80000080;
	cpu->next_next_pc = cpu->next_pc + 4;
}

static inline void cpu_reg_set(struct HakuyaCPU *cpu, uint32_t reg, uint32_t value) {
	if (reg == REG_ZERO) return;
	cpu->regs[reg] = value;

	if (reg == cpu->pending_load[0].reg) {
		cpu->pending_load[0] = (struct PendingLoad){ 0 };
	}
}

static inline void cpu_reg_set_pending(struct HakuyaCPU *cpu, uint32_t reg, uint32_t value) {
	if (reg == REG_ZERO) return;

	if (reg == cpu->pending_load[0].reg) {
		cpu->pending_load[0] = (struct PendingLoad){ 0 };
	}
	cpu->pending_load[1].reg = reg;
	cpu->pending_load[1].value = value;
}

static inline void advance_pending_loads(struct HakuyaCPU *cpu) {
	cpu->regs[cpu->pending_load[0].reg] = cpu->pending_load[0].value;
	cpu->pending_load[0] = cpu->pending_load[1];
	cpu->pending_load[1] = (struct PendingLoad){ 0 };
}

void cpu_init(struct HakuyaCPU *cpu) {
	memset(cpu, 0, sizeof(struct HakuyaCPU));
	cpu->pc = BIOS_START;
	cpu->next_pc = cpu->pc + 4;
	cpu->next_next_pc = cpu->next_pc + 4;
}

void cpu_print_regs(struct HakuyaCPU *cpu) {
	printf("PC: %08X\n", cpu->pc);
	for (int i = 0; i < 32; i += 4) {
		printf("R%02d = %08X  R%02d = %08X  R%02d = %08X  R%02d = %08X\n",
		       i+0, cpu->regs[i+0], i+1, cpu->regs[i+1],
		       i+2, cpu->regs[i+2], i+3, cpu->regs[i+3]);
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

static inline void branch(struct HakuyaCPU *cpu, int32_t offset) {
	int32_t target = (int32_t)cpu->pc + 4 + offset * 4;
	cpu->next_next_pc = (uint32_t)target;
}

static inline void op_bltz(struct HakuyaCPU *cpu, struct InstructionTypeI ins) {
	int32_t imm = (int32_t)(int16_t)ins.immediate;
	if ((int32_t)cpu->regs[ins.rs] < 0)
		branch(cpu, imm);
}

static inline void op_bgez(struct HakuyaCPU *cpu, struct InstructionTypeI ins) {
	int32_t imm = (int32_t)(int16_t)ins.immediate;
	if ((int32_t)cpu->regs[ins.rs] >= 0)
		branch(cpu, imm);
}

static inline void op_bltzal(struct HakuyaCPU *cpu, struct InstructionTypeI ins) {
	int32_t imm = (int32_t)(int16_t)ins.immediate;
	if ((int32_t)cpu->regs[ins.rs] < 0)
		branch(cpu, imm);
	cpu_reg_set(cpu, REG_RA, cpu->pc + 8);
}

static inline void op_bgezal(struct HakuyaCPU *cpu, struct InstructionTypeI ins) {
	int32_t imm = (int32_t)(int16_t)ins.immediate;
	if ((int32_t)cpu->regs[ins.rs] >= 0)
		branch(cpu, imm);
	cpu_reg_set(cpu, REG_RA, cpu->pc + 8);
}

static inline void op_beq(struct HakuyaCPU *cpu, struct InstructionTypeI ins) {
	int32_t imm = (int32_t)(int16_t)ins.immediate;
	if (cpu->regs[ins.rs] == cpu->regs[ins.rt])
		branch(cpu, imm);
}

static inline void op_bne(struct HakuyaCPU *cpu, struct InstructionTypeI ins) {
	int32_t imm = (int32_t)(int16_t)ins.immediate;
	if (cpu->regs[ins.rs] != cpu->regs[ins.rt])
		branch(cpu, imm);
}

static inline void op_blez(struct HakuyaCPU *cpu, struct InstructionTypeI ins) {
	int32_t imm = (int32_t)(int16_t)ins.immediate;
	if ((int32_t)cpu->regs[ins.rs] <= 0)
		branch(cpu, imm);
}

static inline void op_bgtz(struct HakuyaCPU *cpu, struct InstructionTypeI ins) {
	int32_t imm = (int32_t)(int16_t)ins.immediate;
	if ((int32_t)cpu->regs[ins.rs] > 0)
		branch(cpu, imm);
}

static inline void op_addi(struct HakuyaCPU *cpu, struct InstructionTypeI ins) {
	int32_t a = (int32_t)(int16_t)ins.immediate;
	int32_t b = (int32_t)cpu->regs[ins.rs];
	int32_t result;
	if (overflow_add(a, b, &result)) {
		raise_exception(cpu, EXCEPTION_OVERFLOW);
		return;
	}
	cpu_reg_set(cpu, ins.rt, (uint32_t)result);
}

static inline void op_addiu(struct HakuyaCPU *cpu, struct InstructionTypeI ins) {
	cpu_reg_set(cpu, ins.rt, cpu->regs[ins.rs] + (uint32_t)(int32_t)(int16_t)ins.immediate);
}

static inline void op_slti(struct HakuyaCPU *cpu, struct InstructionTypeI ins) {
	int32_t imm = (int32_t)(int16_t)ins.immediate;
	cpu_reg_set(cpu, ins.rt, (int32_t)cpu->regs[ins.rs] < imm);
}

static inline void op_sltiu(struct HakuyaCPU *cpu, struct InstructionTypeI ins) {
	uint32_t imm = (uint32_t)(int32_t)(int16_t)ins.immediate;
	cpu_reg_set(cpu, ins.rt, cpu->regs[ins.rs] < imm);
}

static inline void op_andi(struct HakuyaCPU *cpu, struct InstructionTypeI ins) {
	cpu_reg_set(cpu, ins.rt, cpu->regs[ins.rs] & (uint32_t)ins.immediate);
}

static inline void op_ori(struct HakuyaCPU *cpu, struct InstructionTypeI ins) {
	cpu_reg_set(cpu, ins.rt, cpu->regs[ins.rs] | (uint32_t)ins.immediate);
}

static inline void op_xori(struct HakuyaCPU *cpu, struct InstructionTypeI ins) {
	cpu_reg_set(cpu, ins.rt, cpu->regs[ins.rs] ^ (uint32_t)ins.immediate);
}

static inline void op_lui(struct HakuyaCPU *cpu, struct InstructionTypeI ins) {
	cpu_reg_set(cpu, ins.rt, (uint32_t)ins.immediate << 16);
}

static inline void op_lb(struct HakuyaCPU *cpu, struct InstructionTypeI ins) {
	uint32_t address = cpu->regs[ins.rs] + (uint32_t)(int32_t)(int16_t)ins.immediate;
	if ((cpu->cop0_regs[12] & 0x00010000) != 0) {
		/* fprintf(stderr, "[WARNING] Load at %08X was ignored since cache is isolated\n", address); */
		return;
	}
	cpu_reg_set_pending(cpu, ins.rt, (uint32_t)(int32_t)(int8_t)bus_read8(address));
}

static inline void op_lh(struct HakuyaCPU *cpu, struct InstructionTypeI ins) {
	uint32_t address = cpu->regs[ins.rs] + (uint32_t)(int32_t)(int16_t)ins.immediate;
	if (address % 2 != 0) {
		raise_exception(cpu, EXCEPTION_ADDRESS_LOAD_ERROR);
		return;
	}
	if ((cpu->cop0_regs[12] & 0x00010000) != 0) {
		/* fprintf(stderr, "[WARNING] Store at %08X was ignored since cache is isolated\n", address); */
		return;
	}
	cpu_reg_set_pending(cpu, ins.rt, (uint32_t)(int32_t)(int16_t)bus_read16(address));
}

static inline void op_lwl(struct HakuyaCPU *cpu, struct InstructionTypeI ins) {
	uint32_t address = cpu->regs[ins.rs] + (uint32_t)(int32_t)(int16_t)ins.immediate;
	uint32_t aligned_address = address & (~(uint32_t)0x03);
	uint32_t value = bus_read32(aligned_address);
	uint32_t result;
	if (cpu->pending_load[0].reg == ins.rt) {
		result = cpu->pending_load[0].value;
		cpu->pending_load[0] = (struct PendingLoad){ 0 };
	} else {
		result = cpu->regs[ins.rt];
	}

	switch (address % 4) {
	case 0: result &= 0x00FFFFFF; result |= (value << 24); break;
	case 1: result &= 0x0000FFFF; result |= (value << 16); break;
	case 2: result &= 0x000000FF; result |= (value <<  8); break;
	case 3: result &= 0x00000000; result |= (value <<  0); break;
	}

	cpu_reg_set_pending(cpu, ins.rt, result);
}

static inline void op_lwr(struct HakuyaCPU *cpu, struct InstructionTypeI ins) {
	uint32_t address = cpu->regs[ins.rs] + (uint32_t)(int32_t)(int16_t)ins.immediate;
	uint32_t aligned_address = address & (~(uint32_t)0x03);
	uint32_t value = bus_read32(aligned_address);
	uint32_t result;
	if (cpu->pending_load[0].reg == ins.rt) {
		result = cpu->pending_load[0].value;
		cpu->pending_load[0] = (struct PendingLoad){ 0 };
	} else {
		result = cpu->regs[ins.rt];
	}

	switch (address % 4) {
	case 0: result &= 0x00000000; result |= (value >>  0); break;
	case 1: result &= 0xFF000000; result |= (value >>  8); break;
	case 2: result &= 0xFFFF0000; result |= (value >> 16); break;
	case 3: result &= 0xFFFFFF00; result |= (value >> 24); break;
	}

	cpu_reg_set_pending(cpu, ins.rt, result);
}

static inline void op_swl(struct HakuyaCPU *cpu, struct InstructionTypeI ins) {
	uint32_t address = cpu->regs[ins.rs] + (uint32_t)(int32_t)(int16_t)ins.immediate;
	uint32_t aligned_address = address & (~(uint32_t)0x03);
	uint32_t value  = cpu->regs[ins.rt];
	uint32_t result = bus_read32(aligned_address);

	switch (address % 4) {
	case 0: result &= 0xFFFFFF00; result |= (value >> 24); break;
	case 1: result &= 0xFFFF0000; result |= (value >> 16); break;
	case 2: result &= 0xFF000000; result |= (value >>  8); break;
	case 3: result &= 0x00000000; result |= (value >>  0); break;
	}

	bus_write32(aligned_address, result);
}

static inline void op_swr(struct HakuyaCPU *cpu, struct InstructionTypeI ins) {
	uint32_t address = cpu->regs[ins.rs] + (uint32_t)(int32_t)(int16_t)ins.immediate;
	uint32_t aligned_address = address & (~(uint32_t)0x03);
	uint32_t value  = cpu->regs[ins.rt];
	uint32_t result = bus_read32(aligned_address);

	switch (address % 4) {
	case 0: result &= 0x00000000; result |= (value <<  0); break;
	case 1: result &= 0x000000FF; result |= (value <<  8); break;
	case 2: result &= 0x0000FFFF; result |= (value << 16); break;
	case 3: result &= 0x00FFFFFF; result |= (value << 24); break;
	}

	bus_write32(aligned_address, result);
}

static inline void op_lw(struct HakuyaCPU *cpu, struct InstructionTypeI ins) {
	uint32_t address = cpu->regs[ins.rs] + (uint32_t)(int32_t)(int16_t)ins.immediate;
	if (address % 4 != 0) {
		raise_exception(cpu, EXCEPTION_ADDRESS_LOAD_ERROR);
		return;
	}
	if ((cpu->cop0_regs[12] & 0x00010000) != 0) {
		/* fprintf(stderr, "[WARNING] Load at %08X was ignored since cache is isolated\n", address); */
		return;
	}
	cpu_reg_set_pending(cpu, ins.rt, bus_read32(address));
}

static inline void op_lbu(struct HakuyaCPU *cpu, struct InstructionTypeI ins) {
	uint32_t address = cpu->regs[ins.rs] + (uint32_t)(int32_t)(int16_t)ins.immediate;
	if ((cpu->cop0_regs[12] & 0x00010000) != 0) {
		/* fprintf(stderr, "[WARNING] Load at %08X was ignored since cache is isolated\n", address); */
		return;
	}
	cpu_reg_set_pending(cpu, ins.rt, (uint32_t)bus_read8(address));
}

static inline void op_lhu(struct HakuyaCPU *cpu, struct InstructionTypeI ins) {
	uint32_t address = cpu->regs[ins.rs] + (uint32_t)(int32_t)(int16_t)ins.immediate;
	if (address % 2 != 0) {
		raise_exception(cpu, EXCEPTION_ADDRESS_LOAD_ERROR);
		return;
	}
	if ((cpu->cop0_regs[12] & 0x00010000) != 0) {
		/* fprintf(stderr, "[WARNING] Load at %08X was ignored since cache is isolated\n", address); */
		return;
	}
	cpu_reg_set_pending(cpu, ins.rt, (uint32_t)bus_read16(address));
}

static inline void op_sb(struct HakuyaCPU *cpu, struct InstructionTypeI ins) {
	uint32_t address = cpu->regs[ins.rs] + (uint32_t)(int32_t)(int16_t)ins.immediate;
	if ((cpu->cop0_regs[12] & 0x00010000) != 0) {
		/* fprintf(stderr, "[WARNING] Store at %08X was ignored since cache is isolated\n", address); */
		return;
	}
	bus_write8(address, cpu->regs[ins.rt] & 0xFF);
}

static inline void op_sh(struct HakuyaCPU *cpu, struct InstructionTypeI ins) {
	uint32_t address = cpu->regs[ins.rs] + (uint32_t)(int32_t)(int16_t)ins.immediate;
	if (address % 2 != 0) {
		raise_exception(cpu, EXCEPTION_ADDRESS_STORE_ERROR);
		return;
	}
	if ((cpu->cop0_regs[12] & 0x00010000) != 0) {
		/* fprintf(stderr, "[WARNING] Store at %08X was ignored since cache is isolated\n", address); */
		return;
	}
	bus_write16(address, cpu->regs[ins.rt] & 0xFFFF);
}

static inline void op_sw(struct HakuyaCPU *cpu, struct InstructionTypeI ins) {
	uint32_t address = cpu->regs[ins.rs] + (uint32_t)(int32_t)(int16_t)ins.immediate;
	if (address % 4 != 0) {
		raise_exception(cpu, EXCEPTION_ADDRESS_STORE_ERROR);
		return;
	}
	if ((cpu->cop0_regs[12] & 0x00010000) != 0) {
		/* fprintf(stderr, "[WARNING] Store at %08X was ignored since cache is isolated\n", address); */
		return;
	}
	bus_write32(address, cpu->regs[ins.rt]);
}

static inline void op_lwc0(struct HakuyaCPU *cpu, struct InstructionTypeI ins) {
	(void)ins;
	raise_exception(cpu, EXCEPTION_COPROCESSOR_UNUSABLE);
}

static inline void op_lwc1(struct HakuyaCPU *cpu, struct InstructionTypeI ins) {
	(void)ins;
	raise_exception(cpu, EXCEPTION_COPROCESSOR_UNUSABLE);
}

static inline void op_lwc2(struct HakuyaCPU *cpu, struct InstructionTypeI ins) {
	(void)cpu;
	(void)ins;
	PANIC("Error: %s", "Unimplemented COP2 load word instruction");
}

static inline void op_lwc3(struct HakuyaCPU *cpu, struct InstructionTypeI ins) {
	(void)ins;
	raise_exception(cpu, EXCEPTION_COPROCESSOR_UNUSABLE);
}

static inline void op_swc0(struct HakuyaCPU *cpu, struct InstructionTypeI ins) {
	(void)ins;
	raise_exception(cpu, EXCEPTION_COPROCESSOR_UNUSABLE);
}

static inline void op_swc1(struct HakuyaCPU *cpu, struct InstructionTypeI ins) {
	(void)ins;
	raise_exception(cpu, EXCEPTION_COPROCESSOR_UNUSABLE);
}

static inline void op_swc2(struct HakuyaCPU *cpu, struct InstructionTypeI ins) {
	(void)cpu;
	(void)ins;
	PANIC("Error: %s", "Unimplemented COP2 store word instruction");
}

static inline void op_swc3(struct HakuyaCPU *cpu, struct InstructionTypeI ins) {
	(void)ins;
	raise_exception(cpu, EXCEPTION_COPROCESSOR_UNUSABLE);
}

static inline void op_sll(struct HakuyaCPU *cpu, struct InstructionTypeR ins) {
	cpu_reg_set(cpu, ins.rd, cpu->regs[ins.rt] << ins.shamt);
}

static inline void op_srl(struct HakuyaCPU *cpu, struct InstructionTypeR ins) {
	cpu_reg_set(cpu, ins.rd, cpu->regs[ins.rt] >> ins.shamt);
}

static inline void op_sra(struct HakuyaCPU *cpu, struct InstructionTypeR ins) {
	// Get guaranteed arithmetic right shift behavior. On gcc -O2, this
	// converts into a single sra instruction as desired.
	int32_t signed_num = (int32_t)cpu->regs[ins.rt];
	if (signed_num < 0)
		signed_num = ~(~signed_num >> ins.shamt);
	else
		signed_num = signed_num >> ins.shamt;
	cpu_reg_set(cpu, ins.rd, (uint32_t)signed_num);
}

static inline void op_sllv(struct HakuyaCPU *cpu, struct InstructionTypeR ins) {
	uint32_t shift_amount = cpu->regs[ins.rs] & 0x1F;
	cpu_reg_set(cpu, ins.rd, cpu->regs[ins.rt] << shift_amount);
}

static inline void op_srlv(struct HakuyaCPU *cpu, struct InstructionTypeR ins) {
	uint32_t shift_amount = cpu->regs[ins.rs] & 0x1F;
	cpu_reg_set(cpu, ins.rd, cpu->regs[ins.rt] >> shift_amount);
}

static inline void op_srav(struct HakuyaCPU *cpu, struct InstructionTypeR ins) {
	uint32_t shift_amount = cpu->regs[ins.rs] & 0x1F;
	// Get guaranteed arithmetic right shift behavior. On gcc -O2, this
	// converts into a single sra instruction as desired.
	int32_t signed_num = (int32_t)cpu->regs[ins.rt];
	if (signed_num < 0)
		signed_num = ~(~signed_num >> shift_amount);
	else
		signed_num = signed_num >> shift_amount;
	cpu_reg_set(cpu, ins.rd, (uint32_t)signed_num);
}

static inline void op_jr(struct HakuyaCPU *cpu, struct InstructionTypeR ins) {
	if (cpu->regs[ins.rs] % 4 != 0) {
		raise_exception(cpu, EXCEPTION_ADDRESS_LOAD_ERROR);
		return;
	}
	cpu->next_next_pc = cpu->regs[ins.rs];
}

static inline void op_jalr(struct HakuyaCPU *cpu, struct InstructionTypeR ins) {
	uint32_t jump_target = cpu->regs[ins.rs];
	cpu_reg_set(cpu, ins.rd, cpu->pc + 8);
	if (jump_target % 4 != 0)
	{
		raise_exception(cpu, EXCEPTION_ADDRESS_LOAD_ERROR);
		return;
	}
	cpu->next_next_pc = jump_target;
}

static inline void op_syscall(struct HakuyaCPU *cpu, struct InstructionTypeR ins) {
	(void)ins;
	raise_exception(cpu, EXCEPTION_SYSCALL);
}

static inline void op_break(struct HakuyaCPU *cpu, struct InstructionTypeR ins) {
	(void)ins;
	raise_exception(cpu, EXCEPTION_BREAK);
}

static inline void op_mfhi(struct HakuyaCPU *cpu, struct InstructionTypeR ins) {
	cpu_reg_set(cpu, ins.rd, cpu->hi);
}

static inline void op_mthi(struct HakuyaCPU *cpu, struct InstructionTypeR ins) {
	cpu->hi = cpu->regs[ins.rs];
}

static inline void op_mflo(struct HakuyaCPU *cpu, struct InstructionTypeR ins) {
	cpu_reg_set(cpu, ins.rd, cpu->lo);
}

static inline void op_mtlo(struct HakuyaCPU *cpu, struct InstructionTypeR ins) {
	cpu->lo = cpu->regs[ins.rs];
}

static inline void op_mult(struct HakuyaCPU *cpu, struct InstructionTypeR ins) {
	int64_t a = (int64_t)(int32_t)cpu->regs[ins.rs];
	int64_t b = (int64_t)(int32_t)cpu->regs[ins.rt];
	uint64_t result = (uint64_t)(a * b);
	cpu->lo = (result & 0xFFFFFFFF);
	cpu->hi = (result >> 32);
}

static inline void op_multu(struct HakuyaCPU *cpu, struct InstructionTypeR ins) {
	uint64_t a = (uint64_t)cpu->regs[ins.rs];
	uint64_t b = (uint64_t)cpu->regs[ins.rt];
	uint64_t result = a * b;
	cpu->lo = (result & 0xFFFFFFFF);
	cpu->hi = (result >> 32);
}

static inline void op_div(struct HakuyaCPU *cpu, struct InstructionTypeR ins) {
	int32_t num = (int32_t)cpu->regs[ins.rs];
	int32_t dem = (int32_t)cpu->regs[ins.rt];
	if (dem == 0) {
		cpu->hi = (uint32_t)0;
		cpu->lo = (uint32_t)0;
		cpu->hi = (uint32_t)num;
		cpu->lo = (num < 0) ? 0x00000001u : 0xFFFFFFFFu;
	} else if (num == INT32_MIN && dem == -1) {
		cpu->hi = 0x00000000u;
		cpu->lo = 0x80000000u;
	} else {
		cpu->lo = (uint32_t)(num / dem);
		cpu->hi = (uint32_t)(num % dem);
	}
}

static inline void op_divu(struct HakuyaCPU *cpu, struct InstructionTypeR ins) {
	uint32_t num = cpu->regs[ins.rs];
	uint32_t dem = cpu->regs[ins.rt];
	if (dem == 0) {
		cpu->hi = num;
		cpu->lo = 0xFFFFFFFFu;
	} else {
		cpu->lo = (uint32_t)(num / dem);
		cpu->hi = (uint32_t)(num % dem);
	}
}

static inline void op_add(struct HakuyaCPU *cpu, struct InstructionTypeR ins) {
	int32_t a = (int32_t)cpu->regs[ins.rs];
	int32_t b = (int32_t)cpu->regs[ins.rt];
	int32_t result;
	if (overflow_add(a, b, &result)) {
		raise_exception(cpu, EXCEPTION_OVERFLOW);
		return;
	}
	cpu_reg_set(cpu, ins.rd, (uint32_t)result);
}

static inline void op_addu(struct HakuyaCPU *cpu, struct InstructionTypeR ins) {
	cpu_reg_set(cpu, ins.rd, cpu->regs[ins.rs] + cpu->regs[ins.rt]);
}

static inline void op_sub(struct HakuyaCPU *cpu, struct InstructionTypeR ins) {
	int32_t a = (int32_t)cpu->regs[ins.rs];
	int32_t b = (int32_t)cpu->regs[ins.rt];
	int32_t result;
	if (overflow_sub(a, b, &result)) {
		raise_exception(cpu, EXCEPTION_OVERFLOW);
		return;
	}
	cpu_reg_set(cpu, ins.rd, (uint32_t)result);
}

static inline void op_subu(struct HakuyaCPU *cpu, struct InstructionTypeR ins) {
	cpu_reg_set(cpu, ins.rd, cpu->regs[ins.rs] - cpu->regs[ins.rt]);
}

static inline void op_and(struct HakuyaCPU *cpu, struct InstructionTypeR ins) {
	cpu_reg_set(cpu, ins.rd, cpu->regs[ins.rs] & cpu->regs[ins.rt]);
}

static inline void op_or(struct HakuyaCPU *cpu, struct InstructionTypeR ins) {
	cpu_reg_set(cpu, ins.rd, cpu->regs[ins.rs] | cpu->regs[ins.rt]);
}

static inline void op_xor(struct HakuyaCPU *cpu, struct InstructionTypeR ins) {
	cpu_reg_set(cpu, ins.rd, cpu->regs[ins.rs] ^ cpu->regs[ins.rt]);
}

static inline void op_nor(struct HakuyaCPU *cpu, struct InstructionTypeR ins) {
	cpu_reg_set(cpu, ins.rd, ~(cpu->regs[ins.rs] | cpu->regs[ins.rt]));
}

static inline void op_slt(struct HakuyaCPU *cpu, struct InstructionTypeR ins) {
	cpu_reg_set(cpu, ins.rd, (int32_t)cpu->regs[ins.rs] < (int32_t)cpu->regs[ins.rt]);
}

static inline void op_sltu(struct HakuyaCPU *cpu, struct InstructionTypeR ins) {
	cpu_reg_set(cpu, ins.rd, cpu->regs[ins.rs] < cpu->regs[ins.rt]);
}

static inline void op_j(struct HakuyaCPU *cpu, struct InstructionTypeJ ins) {
	cpu->next_next_pc = (cpu->next_pc & 0xF0000000) | (ins.target << 2);
}

static inline void op_jal(struct HakuyaCPU *cpu, struct InstructionTypeJ ins) {
	cpu_reg_set(cpu, REG_RA, cpu->pc + 8);
	cpu->next_next_pc = (cpu->next_pc & 0xF0000000) | (ins.target << 2);
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

static void execute_r_instruction(struct HakuyaCPU *cpu, uint32_t instruction) {
	struct InstructionTypeR ins = {
		.opcode = (uint8_t)bit_slice(instruction, 31, 26),
		.rs     = (uint8_t)bit_slice(instruction, 25, 21),
		.rt     = (uint8_t)bit_slice(instruction, 20, 16),
		.rd     = (uint8_t)bit_slice(instruction, 15, 11),
		.shamt  = (uint8_t)bit_slice(instruction, 10,  6),
		.funct  = (uint8_t)bit_slice(instruction,  5,  0),
	};

	switch (ins.funct) {
	case 0x00: op_sll (cpu, ins); break;
	// 0x01 is unused
	case 0x02: op_srl (cpu, ins); break;
	case 0x03: op_sra (cpu, ins); break;
	case 0x04: op_sllv(cpu, ins); break;
	// 0x05 is unused
	case 0x06: op_srlv(cpu, ins); break;
	case 0x07: op_srav(cpu, ins); break;
	case 0x08: op_jr  (cpu, ins); break;
	case 0x09: op_jalr(cpu, ins); break;
	// 0x0A and 0x0B are unused
	case 0x0C: op_syscall(cpu, ins); break;
	case 0x0D: op_break  (cpu, ins); break;
	// 0x0E and 0x0F are unused
	case 0x10: op_mfhi(cpu, ins); break;
	case 0x11: op_mthi(cpu, ins); break;
	case 0x12: op_mflo(cpu, ins); break;
	case 0x13: op_mtlo(cpu, ins); break;
	// 0x14 ~ 0x17 are unused
	case 0x18: op_mult (cpu, ins); break;
	case 0x19: op_multu(cpu, ins); break;
	case 0x1A: op_div (cpu, ins); break;
	case 0x1B: op_divu(cpu, ins); break;
	// 0x1C ~ 0x1F are unused
	case 0x20: op_add (cpu, ins); break;
	case 0x21: op_addu(cpu, ins); break;
	case 0x22: op_sub (cpu, ins); break;
	case 0x23: op_subu(cpu, ins); break;
	case 0x24: op_and (cpu, ins); break;
	case 0x25: op_or  (cpu, ins); break;
	case 0x26: op_xor (cpu, ins); break;
	case 0x27: op_nor (cpu, ins); break;
	// 0x28 and 0x29 are unused
	case 0x2A: op_slt (cpu, ins); break;
	case 0x2B: op_sltu(cpu, ins); break;
	// 0x2C ~ 0x3F are unused
	default: PANIC("Unknown R instruction (instruction: 0x%08X, funct: 0x%02X)",
		       instruction, ins.funct);
	}
}

static void execute_i_instruction(struct HakuyaCPU *cpu, uint32_t instruction) {
	struct InstructionTypeI ins = {
		.opcode    = (uint8_t)bit_slice(instruction, 31, 26),
		.rs        = (uint8_t)bit_slice(instruction, 25, 21),
		.rt        = (uint8_t)bit_slice(instruction, 20, 16),
		.immediate = (uint16_t)bit_slice(instruction, 15,  0),
	};

	switch (ins.opcode) {
	case 0x01: // The bcond opcodes
		switch (bit_slice(instruction, 20, 16)) {
		case 0x00: op_bltz  (cpu, ins); break;
		case 0x01: op_bgez  (cpu, ins); break;
		case 0x10: op_bltzal(cpu, ins); break;
		case 0x11: op_bgezal(cpu, ins); break;
		default:
			if (instruction & (1u << 16))
				op_bgez(cpu, ins);
			else
				op_bltz(cpu, ins);
			break;
		}
		break;
	// 0x02 and 0x03 are jump codes here
	case 0x04: op_beq  (cpu, ins); break;
	case 0x05: op_bne  (cpu, ins); break;
	case 0x06: op_blez (cpu, ins); break;
	case 0x07: op_bgtz (cpu, ins); break;
	case 0x08: op_addi (cpu, ins); break;
	case 0x09: op_addiu(cpu, ins); break;
	case 0x0A: op_slti (cpu, ins); break;
	case 0x0B: op_sltiu(cpu, ins); break;
	case 0x0C: op_andi (cpu, ins); break;
	case 0x0D: op_ori  (cpu, ins); break;
	case 0x0E: op_xori (cpu, ins); break;
	case 0x0F: op_lui  (cpu, ins); break;
	// 0x10, 0x11, 0x12, and 0x13 are coprocessor codes
	// 0x14 ~ 0x1F are unused
	case 0x20: op_lb   (cpu, ins); break;
	case 0x21: op_lh   (cpu, ins); break;
	case 0x22: op_lwl  (cpu, ins); break;
	case 0x23: op_lw   (cpu, ins); break;
	case 0x24: op_lbu  (cpu, ins); break;
	case 0x25: op_lhu  (cpu, ins); break;
	case 0x26: op_lwr  (cpu, ins); break;
	// 0x27 is unused
	case 0x28: op_sb   (cpu, ins); break;
	case 0x29: op_sh   (cpu, ins); break;
	case 0x2A: op_swl  (cpu, ins); break;
	case 0x2B: op_sw   (cpu, ins); break;
	// 0x2C and 0x2D are unused
	case 0x2E: op_swr  (cpu, ins); break;
	// 0x2F is unused
	case 0x30: op_lwc0(cpu, ins); break;
	case 0x31: op_lwc1(cpu, ins); break;
	case 0x32: op_lwc2(cpu, ins); break;
	case 0x33: op_lwc3(cpu, ins); break;
	// 0x34 ~ 0x37 are unused
	case 0x38: op_swc0(cpu, ins); break;
	case 0x39: op_swc1(cpu, ins); break;
	case 0x3A: op_swc2(cpu, ins); break;
	case 0x3B: op_swc3(cpu, ins); break;
	// 0x3C ~ 0x3F are unused
	default: PANIC("Unknown I instruction (instruction: 0x%08X, opcode: 0x%02X)",
		       instruction, ins.opcode);
	}
}

static void execute_j_instruction(struct HakuyaCPU *cpu, uint32_t instruction) {
	struct InstructionTypeJ ins = {
		.opcode = (uint8_t)bit_slice(instruction, 31, 26),
		.target = (uint32_t)bit_slice(instruction, 25,  0),
	};

	switch (ins.opcode) {
	case 0x02: op_j  (cpu, ins); break;
	case 0x03: op_jal(cpu, ins); break;
	default: PANIC("Unknown J instruction (instruction: 0x%08X, opcode: 0x%02X)",
		       instruction, ins.opcode);
	}
}

static void op_mtc0(struct HakuyaCPU *cpu, uint32_t instruction) {
	uint32_t rt = bit_slice(instruction, 20, 16);
	uint32_t rd = bit_slice(instruction, 15, 11);
	cpu->cop0_regs[rd] = cpu->regs[rt];
}

static void op_mfc0(struct HakuyaCPU *cpu, uint32_t instruction) {
	uint32_t rt = bit_slice(instruction, 20, 16);
	uint32_t rd = bit_slice(instruction, 15, 11);
	cpu_reg_set_pending(cpu, rt, cpu->cop0_regs[rd]);
	/* fprintf(stderr, "[INFO] The value %08X was moved from cop0[%d]\n", cpu->cop0_regs[rd], rd); */
}

static void op_rfe(struct HakuyaCPU *cpu, uint32_t instruction) {
	(void)instruction;
	uint32_t mode = cpu->cop0_regs[REG_SR] & 0x3F;
	cpu->cop0_regs[REG_SR] &= ~(uint32_t)0x0F;
	cpu->cop0_regs[REG_SR] |= (mode >> 2);
}

static void execute_cop0_instruction(struct HakuyaCPU *cpu, uint32_t instruction) {
	uint32_t cop_opcode = bit_slice(instruction, 25, 21);

	switch (cop_opcode) {
	case 0x00: op_mfc0(cpu, instruction); break;
	case 0x02: unimplemented(instruction, "cf0"); break;
	case 0x04: op_mtc0(cpu, instruction); break;
	case 0x06: unimplemented(instruction, "ct0"); break;
	case 0x08: unimplemented(instruction, "bc0"); break;
	case 0x10: // COP0 special instructions
		switch (bit_slice(instruction, 5, 0)) {
		case 0x01: unimplemented(instruction, "tlbr"); break;
		case 0x02: unimplemented(instruction, "tlbwi"); break;
		case 0x06: unimplemented(instruction, "tlbwr"); break;
		case 0x08: unimplemented(instruction, "tlbp"); break;
		case 0x10: op_rfe(cpu, instruction); break;
		default: PANIC("Unknown COP0 sub instruction: 0x%08X", instruction);
		}
		break;
	default: PANIC("Unknown COP0 instruction: 0x%08X", instruction);
	}
}

static void execute_cop2_instruction(struct HakuyaCPU *cpu, uint32_t instruction) {
	(void)cpu;
	PANIC("COP2 is unimplemented. (Instruction: %08X)", instruction);
}

static void execute_instruction(struct HakuyaCPU *cpu, uint32_t instruction) {
	uint32_t opcode = bit_slice(instruction, 31, 26);
	switch (opcode) {
	case 0x00: execute_r_instruction(cpu, instruction);    break;
	case 0x02:
	case 0x03: execute_j_instruction(cpu, instruction);    break;
	case 0x10: execute_cop0_instruction(cpu, instruction); break;
	case 0x11: raise_exception(cpu, EXCEPTION_COPROCESSOR_UNUSABLE); break;
	case 0x12: execute_cop2_instruction(cpu, instruction); break;
	case 0x13: raise_exception(cpu, EXCEPTION_COPROCESSOR_UNUSABLE); break;
	default:   execute_i_instruction(cpu, instruction);    break;
	}
}

static void print_tty_output(struct HakuyaCPU *cpu) {
	uint32_t pc = cpu->pc & 0x1FFFFFFF;
	if (pc == 0xA0 && cpu->regs[9] == 0x3C) {
		putc((char)cpu->regs[4], stdout);
	} else if (pc == 0xB0 && cpu->regs[9] == 0x3D) {
		putc((char)cpu->regs[4], stdout);
	}
}

void cpu_run_next_instruction(struct HakuyaCPU *cpu) {
	advance_pending_loads(cpu);
	uint32_t instruction = bus_read32(cpu->pc);
	execute_instruction(cpu, instruction);
	print_tty_output(cpu);

	cpu->pc = cpu->next_pc;
	cpu->next_pc = cpu->next_next_pc;
	cpu->next_next_pc = cpu->next_pc + 4;
}
