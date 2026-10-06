#include "core.h"

#include <stdlib.h>
#include "cpu.h"

struct HakuyaCore {
	struct HakuyaCPU cpu;
};

struct HakuyaCore *hakuya_core_create(void) {
	struct HakuyaCore *core = malloc(sizeof(struct HakuyaCore));
	cpu_init(&core->cpu);
	return core;
}

void hakuya_core_tick(struct HakuyaCore *core) {
	cpu_run_next_instruction(&core->cpu);
}

void hakuya_core_free(struct HakuyaCore *core) {
	free(core);
}
