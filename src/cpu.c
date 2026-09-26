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
	uint32_t hi;
	uint32_t lo;
	uint32_t regs[32];
	uint32_t next_instruction;
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

static inline uint32_t op_get_source(uint32_t instruction) {
	return (instruction >> 21) & 0x1F;
}

static inline uint32_t op_get_cop_opcode(uint32_t instruction) {
	return (instruction >> 21) & 0x1F;
}

static inline uint32_t op_get_target(uint32_t instruction) {
	return (instruction >> 16) & 0x1F;
}

static inline uint32_t op_get_destination(uint32_t instruction) {
	return (instruction >> 11) & 0x1F;
}

static inline uint32_t op_get_immediate(uint32_t instruction) {
	return instruction & 0xFFFF;
}

static inline int32_t op_get_immediate_signed(uint32_t instruction) {
	int16_t immediate = instruction & 0xFFFF;
	return (int32_t)immediate;
}

static inline int32_t op_get_subfunction(uint32_t instruction) {
	return instruction & 0x3F;
}

static inline int32_t op_get_shift_immediate(uint32_t instruction) {
	return (instruction >> 6) & 0x1F;
}

static inline void branch(struct HakuyaCPU *cpu, int32_t offset) {
	offset  *= 4; // use multiplcation to avoid shifting a signed int
	cpu->pc += offset;
	cpu->pc -= 4; // to compensate for the +4 in run_next_instruction
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

static inline void op_ori(struct HakuyaCPU *cpu, struct InstructionTypeI ins) {
	cpu_reg_set(cpu, ins.rt, cpu->regs[ins.rs] | (uint32_t)ins.immediate);
}

static inline void op_lui(struct HakuyaCPU *cpu, struct InstructionTypeI ins) {
	cpu_reg_set(cpu, ins.rt, (uint32_t)ins.immediate << 16);
}

static inline void op_lw(struct HakuyaCPU *cpu, struct InstructionTypeI ins) {
	uint32_t address = cpu->regs[ins.rs] + (int32_t)(int16_t)ins.immediate;
	if ((cpu->cop0_regs[12] & 0x00010000) != 0) {
		fprintf(stderr, "[WARNING] Load at %08X was ignored since cache is isolated\n", address);
		return;
	}
	cpu_reg_set_pending(cpu, ins.rt, bus_read32(address));
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
	cpu->pc &= 0xF0000000;
	cpu->pc |= (ins.target << 2);
}

static void execute_r_instruction(struct HakuyaCPU *cpu, uint32_t instruction) {
	struct InstructionTypeR ins = {
		.opcode = instruction >> 26,
		.rs     = op_get_source(instruction),
		.rt     = op_get_target(instruction),
		.rd     = op_get_destination(instruction),
		.shamt  = op_get_shift_immediate(instruction),
		.funct  = op_get_subfunction(instruction),
	};

	switch (ins.funct) {
	case 0x00: op_sll (cpu, ins); break;
	case 0x21: op_addu(cpu, ins); break;
	case 0x25: op_or  (cpu, ins); break;
	case 0x2B: op_sltu(cpu, ins); break;
	default: PANIC("Unimplemented subfunction: 0x%02X", ins.funct);
	}
}

static void execute_i_instruction(struct HakuyaCPU *cpu, uint32_t instruction) {
	struct InstructionTypeI ins = {
		.opcode = instruction >> 26,
		.rs     = op_get_source(instruction),
		.rt     = op_get_target(instruction),
		.immediate = instruction & 0xFFFF,
	};

	switch (ins.opcode) {
	case 0x05: op_bne  (cpu, ins); break;
	case 0x08: op_addi (cpu, ins); break;
	case 0x09: op_addiu(cpu, ins); break;
	case 0x0D: op_ori  (cpu, ins); break;
	case 0x0F: op_lui  (cpu, ins); break;
	case 0x23: op_lw   (cpu, ins); break;
	case 0x2B: op_sw   (cpu, ins); break;
	default: PANIC("Unimplemented opcode: 0x%02X", ins.opcode);
	}
}

static void execute_j_instruction(struct HakuyaCPU *cpu, uint32_t instruction) {
	struct InstructionTypeJ ins = {
		.opcode = instruction >> 26,
		.target = instruction & 0x03FFFFFF,
	};

	switch (ins.opcode) {
	case 0x02: op_j(cpu, ins); break;
	default: PANIC("Unimplemented opcode: 0x%02X", ins.opcode);
	}
}

static void op_mtc0(struct HakuyaCPU *cpu, uint32_t instruction) {
	uint32_t t = op_get_target(instruction);
	uint32_t d = op_get_destination(instruction);
	cpu->cop0_regs[d] = cpu->regs[t];
	fprintf(stderr, "[INFO] The value %08X was moved to cop0[%d]\n", cpu->regs[t], d);
}

static void execute_cop0_instruction(struct HakuyaCPU *cpu, uint32_t instruction) {
	uint32_t cop_opcode = op_get_cop_opcode(instruction);

	switch (cop_opcode) {
	case 0x04: op_mtc0(cpu, instruction); break;
	default: PANIC("Unimplemented COP0 instruction: 0x%08X", instruction);
	}
}

static void execute_instruction(struct HakuyaCPU *cpu, uint32_t instruction) {
	uint32_t opcode = instruction >> 26; // top 6 bits
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
	uint32_t instruction = cpu->next_instruction;
	cpu->next_instruction = bus_read32(cpu->pc);
	cpu->pc += 4;
	execute_instruction(cpu, instruction);
}
