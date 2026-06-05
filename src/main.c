#include <stdbool.h>

#include "cpu.h"

int main(void) {
	struct HakuyaCPU *cpu = cpu_init();
	while (true) {
		cpu_print(cpu);
		run_next_instruction(cpu);
	}
	cpu_free(cpu);
}
