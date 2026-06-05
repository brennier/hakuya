#include "cpu.h"
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "bios.h"
#include "debug.h"

#define BIOS_START 0xBFC00000
#define BIOS_SIZE  (512u * 1024u)

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
};

uint8_t mmu_read8(uint32_t address) {
	if ((address & 0xFFF00000) == BIOS_START) {
		return bios_read(address - BIOS_START);
	}

	fprintf(stderr, "Unimplemented address %08X\n", address);
	exit(EXIT_FAILURE);
}

uint32_t mmu_read32(uint32_t address) {
	uint32_t byte0 = mmu_read8(address+0);
	uint32_t byte1 = mmu_read8(address+1);
	uint32_t byte2 = mmu_read8(address+2);
	uint32_t byte3 = mmu_read8(address+3);
	return (byte3 << 24) | (byte2 << 16) | (byte1 << 8) | byte0;
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

static uint32_t op_get_target(uint32_t instruction) {
	return (instruction >> 16) & 0x1F;
}

static uint32_t op_get_source(uint32_t instruction) {
	return (instruction >> 21) & 0x1F;
}

static uint32_t op_get_immediate(uint32_t instruction) {
	return instruction & 0xFFFF;
}

static inline void op_lui(struct HakuyaCPU *cpu, uint32_t instruction) {
	uint32_t immediate = op_get_immediate(instruction);
	uint32_t target    = op_get_target(instruction);
	cpu->regs[target] = (immediate << 16);
}

static inline void op_ori(struct HakuyaCPU *cpu, uint32_t instruction) {
	uint32_t immediate = op_get_immediate(instruction);
	uint32_t target    = op_get_target(instruction);
	uint32_t source    = op_get_source(instruction);
	cpu->regs[target] = (cpu->regs[source] | immediate);
}

static inline void op_sw(struct HakuyaCPU *cpu, uint32_t instruction) {
	uint32_t immediate = op_get_immediate(instruction);
	uint32_t target    = op_get_target(instruction);
	uint32_t source    = op_get_source(instruction);
	uint32_t address   = cpu->regs[source] + immediate;
	uint32_t value     = cpu->regs[target];

	/* mmu_store32(address, target); */
}

static void decode_instruction(struct HakuyaCPU *cpu, uint32_t instruction) {
	uint32_t opcode = op_get_opcode(instruction);

	switch (opcode) {

	case 0x0D: op_ori(cpu, instruction); break;
	case 0x0F: op_lui(cpu, instruction); break;
	case 0x2B: op_sw(cpu, instruction);  break;
	default:   PANIC("Unimplemented instruction: 0x%08X", instruction);
	}
}

void run_next_instruction(struct HakuyaCPU *cpu) {
	uint32_t instruction = mmu_read32(cpu->pc);
	cpu->pc += 4;

	decode_instruction(cpu, instruction);
}
