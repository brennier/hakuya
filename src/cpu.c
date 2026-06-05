#include "cpu.h"
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "bios.h"

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
	uint32_t high;
	uint32_t low;
	uint32_t regs[32];
};

uint8_t mmu_read(uint32_t address) {
	if ((address & 0xFFF00000) == BIOS_START) {
		return bios_read(address - BIOS_START);
	}

	fprintf(stderr, "Unimplemented address %08X\n", address);
	exit(EXIT_FAILURE);
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

static void decode_instruction(struct HakuyaCPU *cpu, uint32_t instruction) {
	// Unimplemented
}

void run_next_instruction(struct HakuyaCPU *cpu) {
	uint32_t instruction = mmu_read(cpu->pc);
	cpu->pc += 4;

	decode_instruction(cpu, instruction);
}
