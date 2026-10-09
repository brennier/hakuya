#include "gpu.h"
#include <stdint.h>
#include <stdio.h>
#include <stdbool.h>

#define VRAM_WIDTH  1024
#define VRAM_HEIGHT 512

enum GPUCommand {
	RESET = 0x00,
	POLY_MONO_OPAQUE      = 0x20,
	POLY_MONO_SEMITRANS   = 0x22,
	QUAD_MONO_OPAQUE      = 0x28,
	QUAD_MONO_SEMITRANS   = 0x2A,
	POLY_SHADED_OPAQUE    = 0x30,
	POLY_SHADED_SEMITRANS = 0x32,
	QUAD_SHADED_OPAQUE    = 0x38,
	QUAD_SHADED_SEMITRANS = 0x3A,
	RECT_MONO_OPAQUE      = 0x60,
	RECT_MONO_SEMITRANS   = 0x62,
	DOT_MONO_OPAQUE       = 0x68,
	DOT_MONO_SEMITRANS    = 0x6A,
	RECT8_MONO_OPAQUE     = 0x70,
	RECT8_MONO_SEMITRANS  = 0x72,
	RECT16_MONO_OPAQUE    = 0x78,
	RECT16_MONO_SEMITRANS = 0x7A,
};

static uint8_t gpu_command_length[0xFF] = {
	[RESET]                 = 1,
	[POLY_MONO_OPAQUE]      = 4,
	[POLY_MONO_SEMITRANS]   = 4,
	[QUAD_MONO_OPAQUE]      = 5,
	[QUAD_MONO_SEMITRANS]   = 5,
	[POLY_SHADED_OPAQUE]    = 6,
	[POLY_SHADED_SEMITRANS] = 6,
	[QUAD_SHADED_OPAQUE]    = 8,
	[QUAD_SHADED_SEMITRANS] = 8,
	[RECT_MONO_OPAQUE]      = 3,
	[RECT_MONO_SEMITRANS]   = 3,
	[DOT_MONO_OPAQUE]       = 2,
	[DOT_MONO_SEMITRANS]    = 2,
	[RECT8_MONO_OPAQUE]     = 2,
	[RECT8_MONO_SEMITRANS]  = 2,
	[RECT16_MONO_OPAQUE]    = 2,
	[RECT16_MONO_SEMITRANS] = 2,
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

static inline uint16_t blend_triangle16(uint16_t c1, uint16_t c2, uint16_t c3,
					float a1, float a2, float a3) {
	uint8_t c1r = (c1 >>  0) & 0x1F;
	uint8_t c1g = (c1 >>  5) & 0x1F;
	uint8_t c1b = (c1 >> 10) & 0x1F;

	uint8_t c2r = (c2 >>  0) & 0x1F;
	uint8_t c2g = (c2 >>  5) & 0x1F;
	uint8_t c2b = (c2 >> 10) & 0x1F;

	uint8_t c3r = (c3 >>  0) & 0x1F;
	uint8_t c3g = (c3 >>  5) & 0x1F;
	uint8_t c3b = (c3 >> 10) & 0x1F;

	uint8_t r = (uint8_t)(c1r * a1 + c2r * a2 + c3r * a3);
	uint8_t g = (uint8_t)(c1g * a1 + c2g * a2 + c3g * a3);
	uint8_t b = (uint8_t)(c1b * a1 + c2b * a2 + c3b * a3);

	return (uint16_t)((b << 10) | (g << 5) | r);
}

// This does floor((a+b)/2)
static inline uint16_t blend_average16(uint16_t a, uint16_t b) {
	return (a & b) + (((a ^ b) >> 1) & 0x3DEFu);
}

static inline uint16_t blend_add16(uint16_t a, uint16_t b) {
	unsigned avg = blend_average16(a,b);
	unsigned sum = ((avg << 1) & 0x7BDEu) | ((a ^ b) & 0x0421u); // (a+b) mod 32 per channel
	unsigned ovf = (avg >> 4) & 0x0421u; // 1 at bit 0 of each overflowed channel
	return (uint16_t)(sum | (ovf * 31u));
}

static inline uint16_t blend_quarter16(uint16_t a, uint16_t b) {
	return blend_add16(a, (b >> 2) & 0x1CE7);
}

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

static inline Vertex read_vertex(uint32_t word) {
	return (Vertex){
		.x = (word >>  0) & 0x3FF,
		.y = (word >> 16) & 0x1FF,
	};
}

void draw_triangle(uint16_t c1, uint16_t c2, uint16_t c3, bool semitrans, Vertex v0, Vertex v1, Vertex v2);
void draw_monochrome_rectangle(uint16_t color, bool semitrans, Vertex pos, Vertex size);

static void gpu_draw_poly_mono(bool semitrans) {
	uint16_t color = color32to16(gpu.word_buffer[0]);
	Vertex v1 = read_vertex(gpu.word_buffer[1]);
	Vertex v2 = read_vertex(gpu.word_buffer[2]);
	Vertex v3 = read_vertex(gpu.word_buffer[3]);
	draw_triangle(color, color, color, semitrans, v1, v2, v3);
}

static void gpu_draw_poly_shaded(bool semitrans) {
	uint16_t c1 = color32to16(gpu.word_buffer[0]);
	Vertex   v1 = read_vertex(gpu.word_buffer[1]);
	uint16_t c2 = color32to16(gpu.word_buffer[2]);
	Vertex   v2 = read_vertex(gpu.word_buffer[3]);
	uint16_t c3 = color32to16(gpu.word_buffer[4]);
	Vertex   v3 = read_vertex(gpu.word_buffer[5]);
	draw_triangle(c1, c2, c3, semitrans, v1, v2, v3);
}

static void gpu_draw_quad_mono(bool semitrans) {
	uint16_t color = color32to16(gpu.word_buffer[0]);
	Vertex v1 = read_vertex(gpu.word_buffer[1]);
	Vertex v2 = read_vertex(gpu.word_buffer[2]);
	Vertex v3 = read_vertex(gpu.word_buffer[3]);
	Vertex v4 = read_vertex(gpu.word_buffer[4]);
	draw_triangle(color, color, color, semitrans, v1, v2, v3);
	draw_triangle(color, color, color, semitrans, v2, v3, v4);
}

static void gpu_draw_quad_shaded(bool semitrans) {
	uint16_t c1 = color32to16(gpu.word_buffer[0]);
	Vertex   v1 = read_vertex(gpu.word_buffer[1]);
	uint16_t c2 = color32to16(gpu.word_buffer[2]);
	Vertex   v2 = read_vertex(gpu.word_buffer[3]);
	uint16_t c3 = color32to16(gpu.word_buffer[4]);
	Vertex   v3 = read_vertex(gpu.word_buffer[5]);
	uint16_t c4 = color32to16(gpu.word_buffer[6]);
	Vertex   v4 = read_vertex(gpu.word_buffer[7]);
	draw_triangle(c1, c2, c3, semitrans, v1, v2, v3);
	draw_triangle(c2, c3, c4, semitrans, v2, v3, v4);
}

static void gpu_draw_rect_mono(int16_t fixed_size, bool semitrans) {
	uint16_t color = color32to16(gpu.word_buffer[0]);
	Vertex pos = read_vertex(gpu.word_buffer[1]);
	Vertex size = (Vertex){ .x = fixed_size, .y = fixed_size };
	if (fixed_size == 0) {
		size = read_vertex(gpu.word_buffer[2]);
	}
	draw_monochrome_rectangle(color, semitrans, pos, size);
}

static void gpu_execute_command(void) {
	switch (gpu.command) {
	case RESET: break;
	case POLY_MONO_OPAQUE:      gpu_draw_poly_mono(false);     break;
	case POLY_MONO_SEMITRANS:   gpu_draw_poly_mono(true);      break;
	case QUAD_MONO_OPAQUE:      gpu_draw_quad_mono(false);     break;
	case QUAD_MONO_SEMITRANS:   gpu_draw_quad_mono(true);      break;
	case POLY_SHADED_OPAQUE:    gpu_draw_poly_shaded(false);   break;
	case POLY_SHADED_SEMITRANS: gpu_draw_poly_shaded(true);    break;
	case QUAD_SHADED_OPAQUE:    gpu_draw_quad_shaded(false);   break;
	case QUAD_SHADED_SEMITRANS: gpu_draw_quad_shaded(true);    break;
	case RECT_MONO_OPAQUE:      gpu_draw_rect_mono(0, false);  break;
	case RECT_MONO_SEMITRANS:   gpu_draw_rect_mono(0, true);   break;
	case DOT_MONO_OPAQUE:       gpu_draw_rect_mono(1, false);  break;
	case DOT_MONO_SEMITRANS:    gpu_draw_rect_mono(1, true);   break;
	case RECT8_MONO_OPAQUE:     gpu_draw_rect_mono(8, false);  break;
	case RECT8_MONO_SEMITRANS:  gpu_draw_rect_mono(8, true);   break;
	case RECT16_MONO_OPAQUE:    gpu_draw_rect_mono(16, false); break;
	case RECT16_MONO_SEMITRANS: gpu_draw_rect_mono(16, true);  break;
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
	if (rel_address == 4) {
		return;
	}

	if (gpu.remaining_words == 0) {
		gpu.command = (value >> 24);
		uint8_t command_length = gpu_command_length[gpu.command];
		if (command_length == 0) {
			fprintf(stderr, "[INFO] Unimplemented GPU0 write with value %08X\n", value);
			return;
		}
		gpu.current_word = 0;
		gpu.remaining_words = command_length;
	}
	gpu.word_buffer[gpu.current_word++] = value;
	gpu.remaining_words--;

	if (gpu.remaining_words == 0) {
		gpu_execute_command();
		gpu.remaining_words = 0;
		gpu.current_word = 0;
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

void draw_monochrome_rectangle(uint16_t color, bool semitrans, Vertex pos, Vertex size) {
	if (semitrans) {
		for (int y = pos.y; y < pos.y + size.y; y++)
		for (int x = pos.x; x < pos.x + size.x; x++) {
			uint16_t *pixel = &gpu.vram[y][x];
			*pixel = blend_average16(color, *pixel);
		}
	} else {
		for (int y = pos.y; y < pos.y + size.y; y++)
		for (int x = pos.x; x < pos.x + size.x; x++)
			gpu.vram[y][x] = color;
	}
}

void draw_triangle(uint16_t c0, uint16_t c1, uint16_t c2,
		   bool semitrans, Vertex v0, Vertex v1, Vertex v2) {
	// Computes the area of the triangle times 2 (used for shading)
	int double_area = vec2_cross(
		vec2_sub(v1, v0),
		vec2_sub(v2, v0)
		);

	// Swap the first two vertices so that the triangle is oriented clockwise
	if (double_area < 0) {
		Vertex v_temp = v0;
		v0 = v1;
		v1 = v_temp;
		uint16_t c_temp = c0;
		c0 = c1;
		c1 = c_temp;
		double_area *= -1;
	}

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
				float a0 = (float)w1 / (float)double_area;
				float a1 = (float)w2 / (float)double_area;
				float a2 = (float)w0 / (float)double_area;
				uint16_t c = blend_triangle16(c0, c1, c2, a0, a1, a2);
				if (semitrans) {
					*pixel = blend_average16(c, *pixel);
				} else {
					*pixel = c;
				}
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
