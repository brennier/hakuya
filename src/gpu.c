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

	uint16_t vram[512][1024];
} gpu;

static inline uint32_t color16to32(uint16_t c) {
	uint32_t c32 = (uint32_t)c;
	uint32_t rb = ((c32 << 9) | (c32 << 3)) & 0x00F800F8u;
	uint32_t g  = (c32 << 6) & 0x0000F800u;
	uint32_t result = 0xFF000000u | rb | g;
	return result | ((result >> 5) & 0x00070707u);
}

static inline uint16_t color32to16(uint32_t c) {
	uint32_t rb = (c & 0x00F800F8u) >> 3;
	uint32_t g  = (c & 0x0000F800u);
	return (uint16_t)(((rb | g) >> 6) | rb);
}

void gpu_render_vram_u32(uint32_t *output_buffer) {
	uint16_t *flat_vram = (uint16_t*)gpu.vram;
	for (int i = 0; i < 512 * 1024; i++)
		output_buffer[i] = color16to32(flat_vram[i]);
}

void gpu_render_vram_u16(uint16_t *output_buffer) {
	uint16_t *flat_vram = (uint16_t*)gpu.vram;
	for (int i = 0; i < 512 * 1024; i++)
		output_buffer[i] = flat_vram[i];
}

static void gpu_draw_monochrome_dot(void) {
	uint16_t pixel = color32to16(gpu.word_buffer[0]);
	uint32_t x = (gpu.word_buffer[1] >>  0) & 0x3FF;
	uint32_t y = (gpu.word_buffer[1] >> 16) & 0x1FF;
	gpu.vram[y][x] = pixel;
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
