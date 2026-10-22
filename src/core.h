#ifndef HAKUYA_CORE_H
#define HAKUYA_CORE_H

#include <stdint.h>
#include <stddef.h>

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

struct HakuyaCore *hakuya_core_create(void);
void hakuya_core_free(struct HakuyaCore *core);

void hakuya_core_tick(struct HakuyaCore *core);
bool hakuya_load_bios(struct HakuyaCore *core, const uint8_t *bios_data, size_t bios_length);
void hakuya_start_exe(struct HakuyaCore *core, uint8_t *exe_data, size_t exe_length);

void hakuya_render_vram_u16(struct HakuyaCore *core, uint16_t *output_buffer);
void hakuya_render_vram_u32(struct HakuyaCore *core, uint32_t *output_buffer);
void hakuya_get_display_area(struct HakuyaCore *core, int *x, int *y, int *width, int *height);

#endif // HAGKUYA_CORE_H
