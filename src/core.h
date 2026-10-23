#ifndef HAKUYA_CORE_H
#define HAKUYA_CORE_H

#include <stdint.h>

#include "cpu.h"
#include "bus.h"
#include "gpu.h"
#include "dma.h"

struct HakuyaCore {
	struct HakuyaCPU cpu;
	uint8_t  bios[512 * 1024]; // 512 KiB
	uint8_t  ram[2 * 1024 * 1024]; // 2 MiB
	uint8_t  scratchpad[1024];
	uint16_t interrupt_status;
	uint16_t interrupt_mask;
	bool bios_is_loaded;
};

#endif // HAKUYA_CORE_H
