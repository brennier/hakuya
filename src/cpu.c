#include "cpu.h"
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>

#include "bios.h"
#include "debug.h"

#define MEM_CONTROL_START 0x1F801000
#define MEM_CONTROL_SIZE  36
#define BIOS_START 0xBFC00000
#define BIOS_SIZE  (512u * 1024u)
#define RAM_SIZE_CONTROL 0x1F801060

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
};

uint8_t mmu_read8(uint32_t address) {
	if ((address & 0xFFF00000) == BIOS_START) {
		return bios_read(address - BIOS_START);
	}

	fprintf(stderr, "Unimplemented address %08X\n", address);
	exit(EXIT_FAILURE);
}

void mmu_store8(uint32_t address, uint8_t value) {
	if (address >= MEM_CONTROL_START && address < MEM_CONTROL_START + MEM_CONTROL_SIZE) {
		fprintf(stderr, "[WARNING] Write to MEM_CONTROL at %08X\n", address);
		return;
	}

	if (address >= RAM_SIZE_CONTROL && address < RAM_SIZE_CONTROL + 4) {
		fprintf(stderr, "[WARNING] Write to RAM_SIZE_CONTROL at %08X\n", address);
		return;
	}

	PANIC("Unhandled store at address %08X\n", address);
}

uint32_t mmu_read32(uint32_t address) {
	assert(address % 4 == 0);
	uint32_t byte0 = mmu_read8(address+0);
	uint32_t byte1 = mmu_read8(address+1);
	uint32_t byte2 = mmu_read8(address+2);
	uint32_t byte3 = mmu_read8(address+3);
	return (byte3 << 24) | (byte2 << 16) | (byte1 << 8) | byte0;
}

void mmu_store32(uint32_t address, uint32_t value) {
	fprintf(stderr, "[WARNING] Write to %08X\n", address);
	assert(address % 4 == 0);
	mmu_store8(address+0, value & 0xFF);
	value >>= 8;
	mmu_store8(address+1, value & 0xFF);
	value >>= 8;
	mmu_store8(address+2, value & 0xFF);
	value >>= 8;
	mmu_store8(address+3, value & 0xFF);
	fprintf(stderr, "\n");
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

static uint32_t op_get_opcode(uint32_t instruction) {
	return instruction >> 26;
}

static uint32_t op_get_source(uint32_t instruction) {
	return (instruction >> 21) & 0x1F;
}

static uint32_t op_get_target(uint32_t instruction) {
	return (instruction >> 16) & 0x1F;
}

static uint32_t op_get_destination(uint32_t instruction) {
	return (instruction >> 11) & 0x1F;
}

static uint32_t op_get_immediate(uint32_t instruction) {
	return instruction & 0xFFFF;
}

static int32_t op_get_immediate_signed(uint32_t instruction) {
	uint16_t immediate = instruction & 0xFFFF;
	return (int32_t)immediate;
}

static int32_t op_get_immediate_jump(uint32_t instruction) {
	return instruction & 0x03FFFFFF;
}

static int32_t op_get_subfunction(uint32_t instruction) {
	return instruction & 0x3F;
}

static int32_t op_get_shift_immediate(uint32_t instruction) {
	return (instruction >> 6) & 0x1F;
}

static int32_t sign_extend16i(uint16_t value) {
	return (int32_t)value;
}

static inline void op_j(struct HakuyaCPU *cpu, uint32_t instruction) {
	uint32_t i = op_get_immediate_jump(instruction);
	cpu->pc &= 0xF0000000;
	cpu->pc |= (i << 2);
}

static inline void op_sll(struct HakuyaCPU *cpu, uint32_t instruction) {
	uint32_t i = op_get_shift_immediate(instruction);
	uint32_t t = op_get_target(instruction);
	uint32_t d = op_get_destination(instruction);
	cpu->regs[d] = (cpu->regs[t] << i);
}

static inline void op_lui(struct HakuyaCPU *cpu, uint32_t instruction) {
	uint32_t i = op_get_immediate(instruction);
	uint32_t t = op_get_target(instruction);
	cpu->regs[t] = (i << 16);
}

static inline void op_ori(struct HakuyaCPU *cpu, uint32_t instruction) {
	uint32_t i = op_get_immediate(instruction);
	uint32_t t = op_get_target(instruction);
	uint32_t s = op_get_source(instruction);
	cpu->regs[t] = (cpu->regs[s] | i);
}

static inline void op_sw(struct HakuyaCPU *cpu, uint32_t instruction) {
	int32_t  i = op_get_immediate_signed(instruction);
	uint32_t t = op_get_target(instruction);
	uint32_t s = op_get_source(instruction);
	uint32_t address = cpu->regs[s] + i;
	uint32_t value   = cpu->regs[t];
	mmu_store32(address, value);
}

static inline void op_addiu(struct HakuyaCPU *cpu, uint32_t instruction) {
	int32_t  i = op_get_immediate_signed(instruction);
	uint32_t t = op_get_target(instruction);
	uint32_t s = op_get_source(instruction);
	cpu->regs[t] = cpu->regs[s] + i;
}

static void decode_subfunction(struct HakuyaCPU *cpu, uint32_t instruction) {
	uint32_t subfunc = op_get_subfunction(instruction);

	switch (subfunc) {
	case 0x00: op_sll(cpu, instruction); break;
	default:   PANIC("Unimplemented subfunction: 0x%08X", instruction);
	}
}

static void decode_instruction(struct HakuyaCPU *cpu, uint32_t instruction) {
	uint32_t opcode = op_get_opcode(instruction);

	switch (opcode) {
	case 0x00: decode_subfunction(cpu, instruction); break;
	case 0x02: op_j(cpu, instruction); break;
	case 0x09: op_addiu(cpu, instruction); break;
	case 0x0D: op_ori(cpu, instruction); break;
	case 0x0F: op_lui(cpu, instruction); break;
	case 0x2B: op_sw(cpu, instruction);  break;
	default:   PANIC("Unimplemented instruction: 0x%08X", instruction);
	}
}

void run_next_instruction(struct HakuyaCPU *cpu) {
	uint32_t instruction = cpu->next_instruction;
	cpu->next_instruction = mmu_read32(cpu->pc);
	cpu->pc += 4;

	decode_instruction(cpu, instruction);
}
