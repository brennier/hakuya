#include "core.h"

#include <stdlib.h>
#include <string.h>

struct HakuyaCore *hakuya_core_create(void) {
	struct HakuyaCore *core = malloc(sizeof(struct HakuyaCore));
	memset(core, 0, sizeof(struct HakuyaCore));
	cpu_init(core);
	return core;
}

void hakuya_core_free(struct HakuyaCore *core) {
	free(core);
}

void hakuya_core_tick(struct HakuyaCore *core) {
	cpu_run_next_instruction(core);
}

bool hakuya_load_bios(struct HakuyaCore *core, const uint8_t *bios_data, size_t bios_length) {
	if (bios_length != sizeof(core->bios))
		return false;
	memcpy(core->bios, bios_data, sizeof(core->bios));
	core->bios_is_loaded = true;
	return true;
}

static inline uint32_t read32_le(uint8_t *data) {
	return ((uint32_t)data[3] << 24)
	     | ((uint32_t)data[2] << 16)
	     | ((uint32_t)data[1] <<  8)
	     | ((uint32_t)data[0] <<  0);
}

void hakuya_start_exe(struct HakuyaCore *core, uint8_t *exe_data, size_t exe_length) {
	(void)exe_length;
	while (core->cpu.pc != 0x80030000) {
		cpu_run_next_instruction(core);
	}

	uint32_t initial_pc   = read32_le(&exe_data[0x10]);
	uint32_t initial_r28  = read32_le(&exe_data[0x14]);
	uint32_t exe_ram_addr = read32_le(&exe_data[0x18]) & 0x1FFFFF;
	uint32_t exe_size     = read32_le(&exe_data[0x1C]);
	uint32_t initial_sp   = read32_le(&exe_data[0x30]);

	memcpy(core->ram + exe_ram_addr, exe_data + 2048, exe_size);
	if (initial_sp) {
		core->cpu.regs[29] = initial_sp;
		core->cpu.regs[30] = initial_sp;
	}
	core->cpu.regs[28] = initial_r28;
	core->cpu.pc = initial_pc;
	core->cpu.next_pc = initial_pc + 4;
	core->cpu.next_next_pc = initial_pc + 8;
}

void hakuya_render_vram_u16(struct HakuyaCore *core, uint16_t *output_buffer) {
	gpu_render_vram_u16(core, output_buffer);
}

void hakuya_render_vram_u32(struct HakuyaCore *core, uint32_t *output_buffer) {
	gpu_render_vram_u32(core, output_buffer);
}

void hakuya_get_display_area(struct HakuyaCore *core, int *x, int *y, int *width, int *height) {
	gpu_get_display_area(core, x, y, width, height);
}
