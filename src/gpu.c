#include "gpu.h"
#include <stdint.h>
#include <stdio.h>
#include <stdbool.h>

#define VRAM_WIDTH  1024
#define VRAM_HEIGHT 512

enum GPUCommand {
	RESET = 0x00,
	MONOCHROME_OPAQUE_POLY = 0x20,
	MONOCHROME_OPAQUE_QUAD = 0x28,
	MONOCHROME_DOT = 0x68,
};

typedef struct {
	int16_t x;
	int16_t y;
} Vertex;

struct HakuyaGPU {
	enum GPUCommand command;
	uint32_t word_buffer[12];
	uint8_t  current_word;
	uint8_t  remaining_words;

	uint16_t vram[VRAM_HEIGHT][VRAM_WIDTH];
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
	for (int i = 0; i < VRAM_WIDTH * VRAM_HEIGHT; i++)
		output_buffer[i] = color16to32(flat_vram[i]);
}

void gpu_render_vram_u16(uint16_t *output_buffer) {
	uint16_t *flat_vram = (uint16_t*)gpu.vram;
	for (int i = 0; i < VRAM_WIDTH * VRAM_HEIGHT; i++)
		output_buffer[i] = flat_vram[i];
}

static void gpu_draw_monochrome_dot(void) {
	uint16_t color = color32to16(gpu.word_buffer[0]);
	uint32_t x = (gpu.word_buffer[1] >>  0) & 0x3FF;
	uint32_t y = (gpu.word_buffer[1] >> 16) & 0x1FF;
	gpu.vram[y][x] = color;
}

static inline Vertex read_vertex(uint32_t word) {
	return (Vertex){
		.x = (word >>  0) & 0x3FF,
		.y = (word >> 16) & 0x1FF,
	};
}

void draw_monochrome_triangle(uint16_t color, Vertex v0, Vertex v1, Vertex v2);

static void gpu_draw_monochrome_opaque_poly(void) {
	uint16_t color = color32to16(gpu.word_buffer[0]);
	Vertex v1 = read_vertex(gpu.word_buffer[1]);
	Vertex v2 = read_vertex(gpu.word_buffer[2]);
	Vertex v3 = read_vertex(gpu.word_buffer[3]);
	draw_monochrome_triangle(color, v1, v2, v3);
}

static void gpu_draw_monochrome_opaque_quad(void) {
	uint16_t color = color32to16(gpu.word_buffer[0]);
	Vertex v1 = read_vertex(gpu.word_buffer[1]);
	Vertex v2 = read_vertex(gpu.word_buffer[2]);
	Vertex v3 = read_vertex(gpu.word_buffer[3]);
	Vertex v4 = read_vertex(gpu.word_buffer[4]);
	draw_monochrome_triangle(color, v1, v2, v3);
	draw_monochrome_triangle(color, v3, v2, v4);
}

static void gpu_execute_command(void) {
	switch (gpu.command) {
	case RESET: break;
	case MONOCHROME_DOT: gpu_draw_monochrome_dot(); break;
	case MONOCHROME_OPAQUE_POLY: gpu_draw_monochrome_opaque_poly(); break;
	case MONOCHROME_OPAQUE_QUAD: gpu_draw_monochrome_opaque_quad(); break;
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
			case MONOCHROME_OPAQUE_POLY:
				gpu.current_word = 0;
				gpu.word_buffer[gpu.current_word++] = value;
				gpu.remaining_words = 3;
				break;
			case MONOCHROME_OPAQUE_QUAD:
				gpu.current_word = 0;
				gpu.word_buffer[gpu.current_word++] = value;
				gpu.remaining_words = 4;
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

static inline int find_max(int a, int b, int c) {
	int max = a;
	if (b > max) max = b;
	if (c > max) max = c;
	return max;
}

static inline int find_min(int a, int b, int c) {
	int min = a;
	if (b < min) min = b;
	if (c < min) min = c;
	return min;
}

static inline int vec2_cross(Vertex u, Vertex v) {
	return u.x * v.y - u.y * v.x;
}

static inline Vertex vec2_sub(Vertex u, Vertex v) {
	return (Vertex){
		.x = u.x - v.x,
		.y = u.y - v.y
	};
}

// determines if an edge between to vertices is a left or top edge
// this only works on a clockwise-oriented triangle
static inline bool is_top_or_left(Vertex v1, Vertex v2) {
	Vertex sub = vec2_sub(v2, v1);
	return (sub.y < 0) || (sub.y == 0 && sub.x > 0);
}

void draw_monochrome_triangle(uint16_t color, Vertex v0, Vertex v1, Vertex v2) {
	// Compute the bounding box of the triangle
	int min_x = find_min(v0.x, v1.x, v2.x);
	int min_y = find_min(v0.y, v1.y, v2.y);
	int max_x = find_max(v0.x, v1.x, v2.x);
	int max_y = find_max(v0.y, v1.y, v2.y);

	// Clamp to the dimensions of the canvas
	if (min_x < 0) min_x = 0;
	if (min_y < 0) min_y = 0;
	if (max_x >= VRAM_WIDTH)  max_x = VRAM_WIDTH  - 1;
	if (max_y >= VRAM_HEIGHT) max_y = VRAM_HEIGHT - 1;

	Vertex xy_start = {
		.x = (int16_t)min_x,
		.y = (int16_t)min_y,
	};

	// Compute the w0 starts and the x/y deltas
	int w0_start = vec2_cross(
		vec2_sub(v0, xy_start),
		vec2_sub(v1, xy_start)
		);
	int w0_delta_x = v0.y - v1.y;
	int w0_delta_y = v1.x - v0.x;

	// Compute the w1 starts and the x/y deltas
	int w1_start = vec2_cross(
		vec2_sub(v1, xy_start),
		vec2_sub(v2, xy_start)
		);
	int w1_delta_x = v1.y - v2.y;
	int w1_delta_y = v2.x - v1.x;

	// Compute the w2 starts and the x/y deltas
	int w2_start = vec2_cross(
		vec2_sub(v2, xy_start),
		vec2_sub(v0, xy_start)
		);
	int w2_delta_x = v2.y - v0.y;
	int w2_delta_y = v0.x - v2.x;

	// Compute the edge biases to prevent overlap
	if (is_top_or_left(v0, v1)) w0_start += 1;
	if (is_top_or_left(v1, v2)) w1_start += 1;
	if (is_top_or_left(v2, v0)) w2_start += 1;

	uint16_t *pixel_start = &gpu.vram[min_y][min_x];
	int pixel_delta_y = VRAM_WIDTH;

	bool reached_triangle = false;

	for (int y = min_y; y <= max_y; y++) {
		int w0 = w0_start;
		int w1 = w1_start;
		int w2 = w2_start;
		uint16_t *pixel = pixel_start;
		for (int x = min_x; x <= max_x; x++) {
			bool inside_triangle = w0 > 0 && w1 > 0 && w2 > 0;
			if (inside_triangle) {
				reached_triangle = true;
				*pixel = color;
			} else if (reached_triangle) {
				break;
			}
			w0 += w0_delta_x;
			w1 += w1_delta_x;
			w2 += w2_delta_x;
			pixel++;
		}
		w0_start += w0_delta_y;
		w1_start += w1_delta_y;
		w2_start += w2_delta_y;
		pixel_start += pixel_delta_y;
		reached_triangle = false;
	}
}
