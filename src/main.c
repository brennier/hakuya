#include <stdbool.h>

#include "cpu.h"
#include "bios.h"

#define BIOS_FILE "./bios/SCPH1001.BIN"

int main(void) {
	bios_load(BIOS_FILE);
	struct HakuyaCPU *cpu = cpu_init();
	while (true) {
		run_next_instruction(cpu);
	}
	cpu_free(cpu);
}
