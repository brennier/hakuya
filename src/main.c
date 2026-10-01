#include <stdbool.h>
#include <stdio.h>

#include "cpu.h"
#include "bios.h"

#define BIOS_FILE "./bios/SCPH1001.BIN"

int main(int argc, char *argv[]) {
	switch (argc) {
	case 1: bios_load(BIOS_FILE); break;
	case 2: bios_load(argv[1]);   break;
	default:
		fprintf(stderr, "Usage: hakuya [BIOS_FILE]\n");
		return 1;
	}

	struct HakuyaCPU *cpu = cpu_init();
	while (true) {
		run_next_instruction(cpu);
	}
	cpu_free(cpu);
	return 0;
}
