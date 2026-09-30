#include "cpu.h"
#include <stdbool.h>
#include <stdint.h>
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

struct HakuyaCPU {
	uint32_t pc;
	uint32_t next_pc;
	uint32_t next_next_pc;
	uint32_t hi;
	uint32_t lo;
	uint32_t regs[32];
	uint32_t cop0_regs[32];
	struct {
		uint32_t reg; // if 0, then no pending load
		uint32_t value;
	} pending_load[2];
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

void cpu_reg_set(struct HakuyaCPU *cpu, uint32_t reg, uint32_t value) {
	if (reg == 0) return;
	cpu->regs[reg] = value;

	if (reg == cpu->pending_load[0].reg) {
		cpu->pending_load[0].reg = 0;
	}
}

void cpu_reg_set_pending(struct HakuyaCPU *cpu, uint32_t reg, uint32_t value) {
	cpu->pending_load[1].reg = reg;
	cpu->pending_load[1].value = value;
}

void advance_pending_loads(struct HakuyaCPU *cpu) {
	if (cpu->pending_load[0].reg > 0) {
		cpu->regs[cpu->pending_load[0].reg] = cpu->pending_load[0].value;
	}
	cpu->pending_load[0] = cpu->pending_load[1];
	memset(&cpu->pending_load[1], 0, sizeof(cpu->pending_load[1]));
}

struct HakuyaCPU *cpu_init(void) {
	struct HakuyaCPU *cpu = malloc(sizeof(struct HakuyaCPU));
	cpu_reset(cpu);
	return cpu;
}

void cpu_free(struct HakuyaCPU *cpu) {
	free(cpu);
}

void cpu_reset(struct HakuyaCPU *cpu) {
	memset(cpu, 0, sizeof(struct HakuyaCPU));
	cpu->pc = BIOS_START;
	cpu->next_pc = cpu->pc + 4;
	cpu->next_next_pc = cpu->next_pc + 4;
}

void cpu_print(struct HakuyaCPU *cpu) {
	printf("PC: %08X\n", cpu->pc);
	for (int i = 0; i < 32; i += 4) {
		printf("R%02d = %08X  R%02d = %08X  R%02d = %08X  R%02d = %08X\n",
		       i+0, cpu->regs[i+0], i+1, cpu->regs[i+1],
		       i+2, cpu->regs[i+2], i+3, cpu->regs[i+3]);
	}
	printf("\n");
}

static inline void branch(struct HakuyaCPU *cpu, int32_t offset) {
	cpu->next_next_pc = cpu->pc + 4 + offset * 4;
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

static inline void op_addi(struct HakuyaCPU *cpu, struct InstructionTypeI ins) {
	int32_t imm   = (int32_t)(int16_t)ins.immediate;
	int32_t value = cpu->regs[ins.rs];
	if ((value > 0 && imm > INT_MAX - value) ||
	    (value < 0 && imm < INT_MIN - value)) {
		PANIC("Addition between %08X and %08X caused an overflow!", value, imm);
	}
	cpu_reg_set(cpu, ins.rt, value + imm);
}

static inline void op_addiu(struct HakuyaCPU *cpu, struct InstructionTypeI ins) {
	cpu_reg_set(cpu, ins.rt, cpu->regs[ins.rs] + (int32_t)(int16_t)ins.immediate);
}

static inline void op_andi(struct HakuyaCPU *cpu, struct InstructionTypeI ins) {
	cpu_reg_set(cpu, ins.rt, cpu->regs[ins.rs] & (uint32_t)ins.immediate);
}

static inline void op_ori(struct HakuyaCPU *cpu, struct InstructionTypeI ins) {
	cpu_reg_set(cpu, ins.rt, cpu->regs[ins.rs] | (uint32_t)ins.immediate);
}

static inline void op_lui(struct HakuyaCPU *cpu, struct InstructionTypeI ins) {
	cpu_reg_set(cpu, ins.rt, (uint32_t)ins.immediate << 16);
}

static inline void op_lb(struct HakuyaCPU *cpu, struct InstructionTypeI ins) {
	uint32_t address = cpu->regs[ins.rs] + (int32_t)(int16_t)ins.immediate;
	if ((cpu->cop0_regs[12] & 0x00010000) != 0) {
		fprintf(stderr, "[WARNING] Load at %08X was ignored since cache is isolated\n", address);
		return;
	}
	cpu_reg_set_pending(cpu, ins.rt, (uint32_t)(int8_t)bus_read8(address));
}

static inline void op_lw(struct HakuyaCPU *cpu, struct InstructionTypeI ins) {
	uint32_t address = cpu->regs[ins.rs] + (int32_t)(int16_t)ins.immediate;
	if ((cpu->cop0_regs[12] & 0x00010000) != 0) {
		fprintf(stderr, "[WARNING] Load at %08X was ignored since cache is isolated\n", address);
		return;
	}
	cpu_reg_set_pending(cpu, ins.rt, bus_read32(address));
}

static inline void op_sb(struct HakuyaCPU *cpu, struct InstructionTypeI ins) {
	uint32_t address = cpu->regs[ins.rs] + (int32_t)(int16_t)ins.immediate;
	if ((cpu->cop0_regs[12] & 0x00010000) != 0) {
		fprintf(stderr, "[WARNING] Store at %08X was ignored since cache is isolated\n", address);
		return;
	}
	bus_write8(address, cpu->regs[ins.rt] & 0xFF);
}

static inline void op_sh(struct HakuyaCPU *cpu, struct InstructionTypeI ins) {
	uint32_t address = cpu->regs[ins.rs] + (int32_t)(int16_t)ins.immediate;
	if ((cpu->cop0_regs[12] & 0x00010000) != 0) {
		fprintf(stderr, "[WARNING] Store at %08X was ignored since cache is isolated\n", address);
		return;
	}
	bus_write16(address, cpu->regs[ins.rt] & 0xFFFF);
}

static inline void op_sw(struct HakuyaCPU *cpu, struct InstructionTypeI ins) {
	uint32_t address = cpu->regs[ins.rs] + (int32_t)(int16_t)ins.immediate;
	if ((cpu->cop0_regs[12] & 0x00010000) != 0) {
		fprintf(stderr, "[WARNING] Store at %08X was ignored since cache is isolated\n", address);
		return;
	}
	bus_write32(address, cpu->regs[ins.rt]);
}

static inline void op_sll(struct HakuyaCPU *cpu, struct InstructionTypeR ins) {
	cpu_reg_set(cpu, ins.rd, cpu->regs[ins.rt] << ins.shamt);
}

static inline void op_jr(struct HakuyaCPU *cpu, struct InstructionTypeR ins) {
	cpu->next_next_pc = cpu->regs[REG_RA];
}

static inline void op_jalr(struct HakuyaCPU *cpu, struct InstructionTypeR ins) {
	cpu_reg_set(cpu, REG_RA, cpu->next_pc + 4);
	cpu_reg_set(cpu, ins.rd, cpu->next_pc + 4);
	cpu->next_next_pc = cpu->regs[ins.rs];
}

static inline void op_addu(struct HakuyaCPU *cpu, struct InstructionTypeR ins) {
	cpu_reg_set(cpu, ins.rd, cpu->regs[ins.rs] + cpu->regs[ins.rt]);
}

static inline void op_or(struct HakuyaCPU *cpu, struct InstructionTypeR ins) {
	cpu_reg_set(cpu, ins.rd, cpu->regs[ins.rs] | cpu->regs[ins.rt]);
}

static inline void op_sltu(struct HakuyaCPU *cpu, struct InstructionTypeR ins) {
	cpu_reg_set(cpu, ins.rd, cpu->regs[ins.rs] < cpu->regs[ins.rt]);
}

static inline void op_j(struct HakuyaCPU *cpu, struct InstructionTypeJ ins) {
	cpu->next_next_pc = (cpu->next_pc & 0xF0000000) | (ins.target << 2);
}

static inline void op_jal(struct HakuyaCPU *cpu, struct InstructionTypeJ ins) {
	cpu_reg_set(cpu, REG_RA, cpu->next_pc + 4);
	cpu->next_next_pc = (cpu->next_pc & 0xF0000000) | (ins.target << 2);
}

static inline uint32_t bit_slice(uint32_t num, int hi, int lo) {
	int width = hi - lo + 1;
	assert(width < 32);
	uint32_t mask = (1u << width) - 1;
	return (num >> lo) & mask;
}

static void unimplemented(uint32_t instruction, const char *name) {
	PANIC("The instruction 0x%08X (op_%s) is unimplemented", instruction, name);
}

static void execute_r_instruction(struct HakuyaCPU *cpu, uint32_t instruction) {
	struct InstructionTypeR ins = {
		.opcode = bit_slice(instruction, 31, 26),
		.rs     = bit_slice(instruction, 25, 21),
		.rt     = bit_slice(instruction, 20, 16),
		.rd     = bit_slice(instruction, 15, 11),
		.shamt  = bit_slice(instruction, 10,  6),
		.funct  = bit_slice(instruction,  5,  0),
	};

	switch (ins.funct) {
	case 0x00: op_sll (cpu, ins); break;
	// 0x01 is unused
	case 0x02: unimplemented(instruction, "srl"); break;
	case 0x03: unimplemented(instruction, "sra"); break;
	case 0x04: unimplemented(instruction, "sllv"); break;
	// 0x05 is unused
	case 0x06: unimplemented(instruction, "srlv"); break;
	case 0x07: unimplemented(instruction, "srav"); break;
	case 0x08: op_jr  (cpu, ins); break;
	case 0x09: op_jalr(cpu, ins); break;
	// 0x0A and 0x0B are unused
	case 0x0C: unimplemented(instruction, "syscall"); break;
	case 0x0D: unimplemented(instruction, "break"); break;
	// 0x0E and 0x0F are unused
	case 0x10: unimplemented(instruction, "mfhi"); break;
	case 0x11: unimplemented(instruction, "mthi"); break;
	case 0x12: unimplemented(instruction, "mflo"); break;
	case 0x13: unimplemented(instruction, "mtlo"); break;
	// 0x14 ~ 0x17 are unused
	case 0x18: unimplemented(instruction, "mult"); break;
	case 0x19: unimplemented(instruction, "multu"); break;
	case 0x1A: unimplemented(instruction, "div"); break;
	case 0x1B: unimplemented(instruction, "divu"); break;
	// 0x1C ~ 0x1F are unused
	case 0x20: unimplemented(instruction, "add"); break;
	case 0x21: op_addu(cpu, ins); break;
	case 0x22: unimplemented(instruction, "sub"); break;
	case 0x23: unimplemented(instruction, "subu"); break;
	case 0x24: unimplemented(instruction, "and"); break;
	case 0x25: op_or  (cpu, ins); break;
	case 0x26: unimplemented(instruction, "xor"); break;
	case 0x27: unimplemented(instruction, "nor"); break;
	// 0x28 and 0x29 are unused
	case 0x2A: unimplemented(instruction, "slt"); break;
	case 0x2B: op_sltu(cpu, ins); break;
	// 0x2C ~ 0x3F are unused
	default: PANIC("Unimplemented R instruction (instruction: 0x%08X, funct: 0x%02X)",
		       instruction, ins.funct);
	}
}

static void execute_i_instruction(struct HakuyaCPU *cpu, uint32_t instruction) {
	struct InstructionTypeI ins = {
		.opcode    = bit_slice(instruction, 31, 26),
		.rs        = bit_slice(instruction, 25, 21),
		.rt        = bit_slice(instruction, 20, 16),
		.immediate = bit_slice(instruction, 15,  0),
	};

	switch (ins.opcode) {
	case 0x01: unimplemented(instruction, "bcond"); break;
	// 0x02 and 0x03 are jump codes here
	case 0x04: op_beq  (cpu, ins); break;
	case 0x05: op_bne  (cpu, ins); break;
	case 0x06: unimplemented(instruction, "blez"); break;
	case 0x07: unimplemented(instruction, "bgtz"); break;
	case 0x08: op_addi (cpu, ins); break;
	case 0x09: op_addiu(cpu, ins); break;
	case 0x0A: unimplemented(instruction, "stli"); break;
	case 0x0B: unimplemented(instruction, "stliu"); break;
	case 0x0C: op_andi (cpu, ins); break;
	case 0x0D: op_ori  (cpu, ins); break;
	case 0x0E: unimplemented(instruction, "xori"); break;
	case 0x0F: op_lui  (cpu, ins); break;
	// 0x10, 0x11, 0x12, and 0x13 are coprocessor codes
	// 0x14 ~ 0x1F are unused
	case 0x20: op_lb   (cpu, ins); break;
	case 0x21: unimplemented(instruction, "lh"); break;
	case 0x22: unimplemented(instruction, "lwl"); break;
	case 0x23: op_lw   (cpu, ins); break;
	case 0x24: unimplemented(instruction, "lbu"); break;
	case 0x25: unimplemented(instruction, "lhu"); break;
	case 0x26: unimplemented(instruction, "lwr"); break;
	// 0x27 is unused
	case 0x28: op_sb   (cpu, ins); break;
	case 0x29: op_sh   (cpu, ins); break;
	case 0x2A: unimplemented(instruction, "swl"); break;
	case 0x2B: op_sw   (cpu, ins); break;
	// 0x2C and 0x2D are unused
	case 0x2E: unimplemented(instruction, "swr"); break;
	// 0x2F is unused
	case 0x30: unimplemented(instruction, "lwc0"); break;
	case 0x31: unimplemented(instruction, "lwc1"); break;
	case 0x32: unimplemented(instruction, "lwc2"); break;
	case 0x33: unimplemented(instruction, "lwc3"); break;
	// 0x34 ~ 0x37 are unused
	case 0x38: unimplemented(instruction, "swc0"); break;
	case 0x39: unimplemented(instruction, "swc1"); break;
	case 0x3A: unimplemented(instruction, "swc2"); break;
	case 0x3B: unimplemented(instruction, "swc3"); break;
	// 0x3C ~ 0x3F are unused
	default: PANIC("Unimplemented I instruction (instruction: 0x%08X, opcode: 0x%02X)",
		       instruction, ins.opcode);
	}
}

static void execute_j_instruction(struct HakuyaCPU *cpu, uint32_t instruction) {
	struct InstructionTypeJ ins = {
		.opcode = bit_slice(instruction, 31, 26),
		.target = bit_slice(instruction, 25,  0),
	};

	switch (ins.opcode) {
	case 0x02: op_j  (cpu, ins); break;
	case 0x03: op_jal(cpu, ins); break;
	default: PANIC("Unimplemented J instruction (instruction: 0x%08X, opcode: 0x%02X)",
		       instruction, ins.opcode);
	}
}

static void op_mtc0(struct HakuyaCPU *cpu, uint32_t instruction) {
	uint32_t rt = bit_slice(instruction, 20, 16);
	uint32_t rd = bit_slice(instruction, 15, 11);
	cpu->cop0_regs[rd] = cpu->regs[rt];
	fprintf(stderr, "[INFO] The value %08X was moved to cop0[%d]\n", cpu->regs[rt], rd);
}

static void op_mfc0(struct HakuyaCPU *cpu, uint32_t instruction) {
	uint32_t rt = bit_slice(instruction, 20, 16);
	uint32_t rd = bit_slice(instruction, 15, 11);
	cpu_reg_set_pending(cpu, rt, cpu->cop0_regs[rd]);
	fprintf(stderr, "[INFO] The value %08X was moved from cop0[%d]\n", cpu->cop0_regs[rd], rd);
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
		switch (bit_slice(instruction, 0, 4)) {
		case 0x01: unimplemented(instruction, "tlbr"); break;
		case 0x02: unimplemented(instruction, "tlbwi"); break;
		case 0x06: unimplemented(instruction, "tlbwr"); break;
		case 0x08: unimplemented(instruction, "tlbp"); break;
		case 0x10: unimplemented(instruction, "fre"); break;
		default: PANIC("Unimplemented COP0 sub instruction: 0x%08X", instruction);
		}
		break;
	default: PANIC("Unimplemented COP0 instruction: 0x%08X", instruction);
	}
}

static void execute_instruction(struct HakuyaCPU *cpu, uint32_t instruction) {
	uint32_t opcode = bit_slice(instruction, 31, 26);
	switch (opcode) {
	case 0x00: execute_r_instruction(cpu, instruction);    break;
	case 0x02:
	case 0x03: execute_j_instruction(cpu, instruction);    break;
	case 0x10: execute_cop0_instruction(cpu, instruction); break;
	default:   execute_i_instruction(cpu, instruction);    break;
	}
}

void run_next_instruction(struct HakuyaCPU *cpu) {
	advance_pending_loads(cpu);
	uint32_t instruction = bus_read32(cpu->pc);
	execute_instruction(cpu, instruction);

	cpu->pc = cpu->next_pc;
	cpu->next_pc = cpu->next_next_pc;
	cpu->next_next_pc = cpu->next_pc + 4;
}
