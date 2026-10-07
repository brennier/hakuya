#include "gpu.h"
#include <stdint.h>
#include <stdio.h>

enum GPUCommand {
	RESET = 0x00,
	MONOCHROME_DOT = 0x68,
};

struct HakuyaGPU {
	enum GPUCommand command;
	uint32_t word_buffer[12];
	uint8_t  current_word;
	uint8_t  remaining_words;

	uint8_t  vram[512][2048];
	uint32_t output[512][1024];
} gpu;

static inline uint32_t convert_color16(uint16_t c) {
	uint32_t result =
		((c << 9) & 0x00F80000) |
		((c << 6) & 0x0000F800) |
		((c << 3) & 0x000000F8);
	result |= (result >> 5) & 0x00070707;
	return result | 0xFF000000;
}

const uint32_t *render_vram(void) {
	uint8_t  *flat_vram   = (uint8_t  *)gpu.vram;
	uint32_t *flat_output = (uint32_t *)gpu.output;
	for (int i = 0; i < 512 * 1024; i++) {
		uint16_t pixel = (uint16_t)((flat_vram[2*i + 1] << 8) | flat_vram[2*i]);
		flat_output[i] = convert_color16(pixel);
	}
	return flat_output;
}

static void gpu_draw_monochrome_dot(void) {
	uint32_t color = gpu.word_buffer[0] & 0x00FFFFFF;
	uint16_t r = (color >> 3)  & 0x1F;
	uint16_t g = (color >> 11) & 0x1F;
	uint16_t b = (color >> 19) & 0x1F;
	uint16_t pixel = (uint16_t)((b << 10) | (g << 5) | (r << 0));
	uint32_t x = (gpu.word_buffer[1] >>  0) & 0xFFFF;
	uint32_t y = (gpu.word_buffer[1] >> 16) & 0xFFFF;
	gpu.vram[y][2 * x + 0] = (pixel >> 0) & 0xFF;
	gpu.vram[y][2 * x + 1] = (pixel >> 8) & 0xFF;
}

static void gpu_execute_command(void) {
	switch (gpu.command) {
	case RESET: break;
	case MONOCHROME_DOT: gpu_draw_monochrome_dot(); break;
	}
}

uint32_t gpu_read(uint32_t address, int bytes) {
	// Always signal that the GPU is ready
	uint32_t result = (1u << 26) | (1u << 27) | (1u << 28);
	fprintf(stderr, "[WARNING] Ignoring read%-3d from %-12s at %08X (returning 0x%08X)\n",
		bytes * 8, "GPU", address, result);
	return result;
}

void gpu_write(uint32_t rel_address, uint32_t value, int bytes) {
	(void)bytes;
	if (rel_address == 0) {
		if (gpu.remaining_words == 0) {
			gpu.command = (value >> 24);
			switch (gpu.command) {
			case RESET:
				gpu.current_word = 0;
				gpu.remaining_words = 0;
				break;
			case MONOCHROME_DOT:
				gpu.current_word = 0;
				gpu.word_buffer[gpu.current_word++] = value;
				gpu.remaining_words = 1;
				break;
			default:
				fprintf(stderr, "[INFO] Unimplemented GPU0 write with value %08X\n", value);
				break;
			}
		} else {
			gpu.word_buffer[gpu.current_word++] = value;
			gpu.remaining_words--;
		}

		if (gpu.current_word != 0 && gpu.remaining_words == 0) {
			gpu_execute_command();
			gpu.remaining_words = 0;
			gpu.current_word = 0;
		}
	}
}
