#ifndef HAKUYA_CPU_H
#define HAKUYA_CPU_H

#include <stdint.h>

struct HakuyaCore;

struct HakuyaCPU {
	uint32_t pc;
	uint32_t next_pc;
	uint32_t next_next_pc;
	uint32_t hi;
	uint32_t lo;
	uint32_t regs[32];
	uint32_t cop0_regs[32];
	struct PendingLoad {
		uint32_t reg; // if 0, then no pending load
		uint32_t value;
	} pending_load[2];
};

void cpu_init(struct HakuyaCore *core);
void cpu_run_next_instruction(struct HakuyaCore *core);
void cpu_print_regs(struct HakuyaCore *core);

#endif // HAGKUYA_CPU_H
