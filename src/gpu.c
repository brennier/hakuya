#include "gpu.h"
#include <stdint.h>
#include <stdio.h>
#include <stdbool.h>
#include <math.h>

#define VRAM_WIDTH  1024
#define VRAM_HEIGHT 512

enum GPUCommand {
	NO_COMMAND            = 0x00, // 0000 0000
	FILL_RECTANGLE        = 0x02, // 0000 0010

	POLY_MONO_OPAQUE      = 0x20, // 0010 0000
	POLY_MONO_SEMITRANS   = 0x22, // 0010 0010
	QUAD_MONO_OPAQUE      = 0x28, // 0010 1000
	QUAD_MONO_SEMITRANS   = 0x2A, // 0010 1010
	QUAD_TEXTURED_OPAQUE_BLENDED = 0x2C, // 0010 1100
	POLY_SHADED_OPAQUE    = 0x30, // 0011 0000
	POLY_SHADED_SEMITRANS = 0x32, // 0011 0010
	QUAD_SHADED_OPAQUE    = 0x38, // 0011 1000
	QUAD_SHADED_SEMITRANS = 0x3A, // 0011 1010

	LINE_MONO_OPAQUE      = 0x40, // 0010 0000
	LINE_MONO_SEMITRANS   = 0x42, // 0010 0010
	POLYLINE_MONO_OPAQUE  = 0x48, // 0010 1000
	POLYLINE_MONO_SEMITRANS = 0x4A, // 0010 1010
	LINE_SHADED_OPAQUE    = 0x50, // 0011 0000
	LINE_SHADED_SEMITRANS = 0x52, // 0011 0010

	RECT_MONO_OPAQUE      = 0x60, // 0110 0000
	RECT_MONO_SEMITRANS   = 0x62, // 0110 0010
	DOT_MONO_OPAQUE       = 0x68, // 0110 1000
	DOT_MONO_SEMITRANS    = 0x6A, // 0110 1010
	RECT8_MONO_OPAQUE     = 0x70, // 0111 0000
	RECT8_MONO_SEMITRANS  = 0x72, // 0111 0010
	RECT16_MONO_OPAQUE    = 0x78, // 0111 1000
	RECT16_MONO_SEMITRANS = 0x7A, // 0111 1010

	VRAM_TO_VRAM_COPY     = 0x80, // 1000 0000
	CPU_TO_VRAM_COPY      = 0xA0, // 1010 0000
	SET_DRAWING_SETTINGS  = 0xE1, // 1110 0001
	SET_DRAWING_AREA_BEG  = 0xE3, // 1110 0011
	SET_DRAWING_AREA_END  = 0xE4, // 1110 0100
};

typedef struct {
	int16_t x;
	int16_t y;
} Vertex;

typedef struct {
	Vertex pos;
	Vertex size;
} Rectangle;

typedef struct {
	Vertex v0;
	Vertex v1;
	Vertex v2;
	uint32_t c0;
	uint32_t c1;
	uint32_t c2;
} Triangle;

struct HakuyaGPU {
	enum GPUCommand command;
	uint32_t word_buffer[12];
	uint8_t  current_word;
	uint8_t  remaining_words;
	Rectangle display_area;
	Vertex drawing_area_beg;
	Vertex drawing_area_end;
	bool dithering_enabled;

	bool cpu_to_vram_copy_mode;
	Vertex polyline_last_vertex;
	uint32_t polyline_last_color;
	bool polyline_semitrans;
	bool polyline_mode;
	Vertex copy_current;
	Vertex copy_start;
	Vertex copy_end;

	uint16_t vram[VRAM_HEIGHT][VRAM_WIDTH];
} gpu;

static inline uint32_t blend_triangle32(uint32_t c1, uint32_t c2, uint32_t c3,
					float a1, float a2, float a3) {
	uint8_t c1r = (c1 >>  0) & 0xFF;
	uint8_t c1g = (c1 >>  8) & 0xFF;
	uint8_t c1b = (c1 >> 16) & 0xFF;

	uint8_t c2r = (c2 >>  0) & 0xFF;
	uint8_t c2g = (c2 >>  8) & 0xFF;
	uint8_t c2b = (c2 >> 16) & 0xFF;

	uint8_t c3r = (c3 >>  0) & 0xFF;
	uint8_t c3g = (c3 >>  8) & 0xFF;
	uint8_t c3b = (c3 >> 16) & 0xFF;

	uint8_t r = (uint8_t)(c1r * a1 + c2r * a2 + c3r * a3);
	uint8_t g = (uint8_t)(c1g * a1 + c2g * a2 + c3g * a3);
	uint8_t b = (uint8_t)(c1b * a1 + c2b * a2 + c3b * a3);

	return (uint32_t)((b << 16) | (g << 8) | r);
}

static inline uint32_t blend_line32(uint32_t c1, uint32_t c2, float a1, float a2) {
	uint8_t c1r = (c1 >>  0) & 0xFF;
	uint8_t c1g = (c1 >>  8) & 0xFF;
	uint8_t c1b = (c1 >> 16) & 0xFF;

	uint8_t c2r = (c2 >>  0) & 0xFF;
	uint8_t c2g = (c2 >>  8) & 0xFF;
	uint8_t c2b = (c2 >> 16) & 0xFF;

	uint8_t r = (uint8_t)(c1r * a1 + c2r * a2);
	uint8_t g = (uint8_t)(c1g * a1 + c2g * a2);
	uint8_t b = (uint8_t)(c1b * a1 + c2b * a2);

	return (uint32_t)((b << 16) | (g << 8) | r);
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

void gpu_get_display_area(int *x, int *y, int *width, int *height) {
	*x = gpu.display_area.pos.x;
	*y = gpu.display_area.pos.y;
	*width  = gpu.display_area.size.x;
	*height = gpu.display_area.size.y;
}

static inline Vertex read_vertex(uint32_t word) {
	return (Vertex){
		.x = (word >>  0) & 0x3FF,
		.y = (word >> 16) & 0x1FF,
	};
}

void draw_monochrome_rectangle(uint32_t color, bool semitrans, Vertex pos, Vertex size);
static inline void draw_triangle(Triangle triangle, bool shaded, bool semitrans);
static inline void draw_line(Vertex v1, Vertex v2, uint32_t c1, uint32_t c2, bool semitrans);

static void gpu_draw_poly_mono(bool semitrans) {
	uint32_t color = gpu.word_buffer[0] & 0x00FFFFFF;
	Vertex v1 = read_vertex(gpu.word_buffer[1]);
	Vertex v2 = read_vertex(gpu.word_buffer[2]);
	Vertex v3 = read_vertex(gpu.word_buffer[3]);
	Triangle triangle = (Triangle){ v1, v2, v3, color, color, color };
	draw_triangle(triangle, false, semitrans);
}

static void gpu_draw_poly_shaded(bool semitrans) {
	uint32_t c1 = gpu.word_buffer[0] & 0x00FFFFFF;
	Vertex   v1 = read_vertex(gpu.word_buffer[1]);
	uint32_t c2 = gpu.word_buffer[2] & 0x00FFFFFF;
	Vertex   v2 = read_vertex(gpu.word_buffer[3]);
	uint32_t c3 = gpu.word_buffer[4] & 0x00FFFFFF;
	Vertex   v3 = read_vertex(gpu.word_buffer[5]);
	Triangle triangle = (Triangle){ v1, v2, v3, c1, c2, c3 };
	draw_triangle(triangle, true, semitrans);
}

static void gpu_draw_quad_mono(bool semitrans) {
	uint32_t color = gpu.word_buffer[0] & 0x00FFFFFF;
	Vertex v1 = read_vertex(gpu.word_buffer[1]);
	Vertex v2 = read_vertex(gpu.word_buffer[2]);
	Vertex v3 = read_vertex(gpu.word_buffer[3]);
	Vertex v4 = read_vertex(gpu.word_buffer[4]);
	Triangle triangle1 = (Triangle){ v1, v2, v3, color, color, color };
	Triangle triangle2 = (Triangle){ v2, v3, v4, color, color, color };
	draw_triangle(triangle1, false, semitrans);
	draw_triangle(triangle2, false, semitrans);
}

static void gpu_draw_quad_shaded(bool semitrans) {
	uint32_t c1 = gpu.word_buffer[0] & 0x00FFFFFF;
	Vertex   v1 = read_vertex(gpu.word_buffer[1]);
	uint32_t c2 = gpu.word_buffer[2] & 0x00FFFFFF;
	Vertex   v2 = read_vertex(gpu.word_buffer[3]);
	uint32_t c3 = gpu.word_buffer[4] & 0x00FFFFFF;
	Vertex   v3 = read_vertex(gpu.word_buffer[5]);
	uint32_t c4 = gpu.word_buffer[6] & 0x00FFFFFF;
	Vertex   v4 = read_vertex(gpu.word_buffer[7]);
	Triangle triangle1 = (Triangle){ v1, v2, v3, c1, c2, c3 };
	Triangle triangle2 = (Triangle){ v2, v3, v4, c2, c3, c4 };
	draw_triangle(triangle1, true, semitrans);
	draw_triangle(triangle2, true, semitrans);
}

static void gpu_draw_quad_textured_blended(bool semitrans) {
	uint32_t c  = gpu.word_buffer[0] & 0x00FFFFFF;
	Vertex   v1 = read_vertex(gpu.word_buffer[1]);
	Vertex   v2 = read_vertex(gpu.word_buffer[3]);
	Vertex   v3 = read_vertex(gpu.word_buffer[5]);
	Vertex   v4 = read_vertex(gpu.word_buffer[7]);
	Triangle triangle1 = (Triangle){ v1, v2, v3, c, c, c };
	Triangle triangle2 = (Triangle){ v2, v3, v4, c, c, c };
	draw_triangle(triangle1, true, semitrans);
	draw_triangle(triangle2, true, semitrans);
}

static void gpu_draw_rect_mono(int16_t fixed_size, bool semitrans) {
	uint32_t color = gpu.word_buffer[0] & 0x00FFFFFF;
	Vertex pos = read_vertex(gpu.word_buffer[1]);
	Vertex size = (Vertex){ .x = fixed_size, .y = fixed_size };
	if (fixed_size == 0) {
		size = read_vertex(gpu.word_buffer[2]);
	}
	draw_monochrome_rectangle(color, semitrans, pos, size);
}

static void gpu_draw_line_mono(bool semitrans) {
	uint32_t color = gpu.word_buffer[0] & 0x00FFFFFF;
	Vertex start = read_vertex(gpu.word_buffer[1]);
	Vertex end   = read_vertex(gpu.word_buffer[2]);
	draw_line(start, end, color, color, semitrans);
}

static void gpu_draw_line_shaded(bool semitrans) {
	uint32_t color1 = gpu.word_buffer[0] & 0x00FFFFFF;
	Vertex start = read_vertex(gpu.word_buffer[1]);
	uint32_t color2 = gpu.word_buffer[2] & 0x00FFFFFF;
	Vertex end   = read_vertex(gpu.word_buffer[3]);
	draw_line(start, end, color1, color2, semitrans);
}

static void gpu_draw_polyline_mono(bool semitrans) {
	uint32_t color = gpu.word_buffer[0] & 0x00FFFFFF;
	Vertex start = read_vertex(gpu.word_buffer[1]);
	Vertex end   = read_vertex(gpu.word_buffer[2]);
	draw_line(start, end, color, color, semitrans);
	gpu.polyline_last_vertex = end;
	gpu.polyline_last_color = color;
	gpu.polyline_semitrans = semitrans;
	gpu.polyline_mode = true;
}

static void gpu_execute_command(void) {
	bool semitrans = gpu.word_buffer[0] & (1u << 25);
	switch (gpu.command) {
	case NO_COMMAND: break;
	case FILL_RECTANGLE: {
		uint32_t color = gpu.word_buffer[0] & 0x00FFFFFF;
		Vertex pos  = read_vertex(gpu.word_buffer[1]);
		pos.x &= 0x03F0;
		Vertex size = read_vertex(gpu.word_buffer[2]);
		size.x += 0x0F;
		size.x &= ~0x0F;
		Vertex end = (Vertex){ .x = pos.x + size.x, .y = pos.y + size.y };

		uint16_t color16 = color32to16(color);
		for (int y = pos.y; y < end.y; y++)
		for (int x = pos.x; x < end.x; x++)
			gpu.vram[y][x] = color16;
		break;
	}
	case POLY_MONO_OPAQUE: case POLY_MONO_SEMITRANS:
		gpu_draw_poly_mono(semitrans);
		break;
	case QUAD_MONO_OPAQUE: case QUAD_MONO_SEMITRANS:
		gpu_draw_quad_mono(semitrans);
		break;
	case POLY_SHADED_OPAQUE: case POLY_SHADED_SEMITRANS:
		gpu_draw_poly_shaded(semitrans);
		break;
	case QUAD_SHADED_OPAQUE: case QUAD_SHADED_SEMITRANS:
		gpu_draw_quad_shaded(semitrans);
		break;
	case QUAD_TEXTURED_OPAQUE_BLENDED:
		gpu_draw_quad_textured_blended(semitrans);
		break;
	case LINE_MONO_OPAQUE: case LINE_MONO_SEMITRANS:
		gpu_draw_line_mono(semitrans);
		break;
	case POLYLINE_MONO_OPAQUE: case POLYLINE_MONO_SEMITRANS:
		gpu_draw_polyline_mono(semitrans);
		break;
	case LINE_SHADED_OPAQUE: case LINE_SHADED_SEMITRANS:
		gpu_draw_line_shaded(semitrans);
		break;
	case RECT_MONO_OPAQUE: case RECT_MONO_SEMITRANS:
		gpu_draw_rect_mono(0, semitrans);
		break;
	case DOT_MONO_OPAQUE: case DOT_MONO_SEMITRANS:
		gpu_draw_rect_mono(1, semitrans);
		break;
	case RECT8_MONO_OPAQUE: case RECT8_MONO_SEMITRANS:
		gpu_draw_rect_mono(8, semitrans);
		break;
	case RECT16_MONO_OPAQUE: case RECT16_MONO_SEMITRANS:
		gpu_draw_rect_mono(16, semitrans);
		break;
	case VRAM_TO_VRAM_COPY: {
		Vertex src  = read_vertex(gpu.word_buffer[1]);
		Vertex dest = read_vertex(gpu.word_buffer[2]);
		Vertex size = read_vertex(gpu.word_buffer[3]);
		if (size.x == 0) size.x = 0x400;
		if (size.y == 0) size.y = 0x200;
		for (int y = 0; y < size.y; y++)
			for (int x = 0; x < size.x; x++) {
				gpu.vram[dest.y + y][dest.x + x] = gpu.vram[src.y + y][src.x + x];
			}
		break;
	}
	case CPU_TO_VRAM_COPY:
		gpu.cpu_to_vram_copy_mode = true;
		gpu.copy_start   = read_vertex(gpu.word_buffer[1]);
		gpu.copy_current = gpu.copy_start;
		Vertex size      = read_vertex(gpu.word_buffer[2]);
		if (size.x == 0) size.x = 0x400;
		if (size.y == 0) size.y = 0x200;
		gpu.copy_end = (Vertex){ gpu.copy_start.x + size.x, gpu.copy_start.y + size.y };
		break;
	case SET_DRAWING_SETTINGS:
		gpu.dithering_enabled = (gpu.word_buffer[0]) & (1u << 9);
		break;
	case SET_DRAWING_AREA_BEG:
		gpu.drawing_area_beg = (Vertex){
			.x = (gpu.word_buffer[0] >>  0) & 0x3FF,
			.y = (gpu.word_buffer[0] >> 10) & 0x1FF,
		};
		break;
	case SET_DRAWING_AREA_END:
		gpu.drawing_area_end = (Vertex){
			.x = (gpu.word_buffer[0] >>  0) & 0x3FF,
			.y = (gpu.word_buffer[0] >> 10) & 0x1FF,
		};
		break;
	}
}

uint32_t gpu_read_gp0(uint32_t address, int bytes) {
	fprintf(stderr, "[WARNING] Ignoring read%-3d from %-12s at %08X (returning 0x%08X)\n",
		bytes * 8, "GPU0", address, 0);
	return 0;
}

uint32_t gpu_read_gp1(uint32_t address, int bytes) {
	// Always signal that the GPU is ready
	uint32_t result = (1u << 26) | (1u << 27) | (1u << 28);
	fprintf(stderr, "[WARNING] Ignoring read%-3d from %-12s at %08X (returning 0x%08X)\n",
		bytes * 8, "GPU1", address, result);
	return result;
}

static inline uint8_t gpu_compute_command_length(uint32_t command) {
	static const uint8_t gpu_command_length[256] = {
		1, 1, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1,  1,  1,  1,  1,
		1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  1,  1,  1,  1,
		4, 4, 4, 4, 7, 7, 7, 7, 5, 5, 5, 5,  9,  9,  9,  9,
		6, 6, 6, 6, 9, 9, 9, 9, 8, 8, 8, 8, 12, 12, 12, 12,
		3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3,  3,  3,  3,  3,
		4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,  4,  4,  4,  4,
		3, 3, 3, 3, 4, 4, 4, 4, 2, 2, 2, 2,  3,  3,  3,  3,
		2, 2, 2, 2, 3, 3, 3, 3, 2, 2, 2, 2,  3,  3,  3,  3,
		4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,  4,  4,  4,  4,
		4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,  4,  4,  4,  4,
		3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3,  3,  3,  3,  3,
		3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3,  3,  3,  3,  3,
		3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3,  3,  3,  3,  3,
		3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3,  3,  3,  3,  3,
		1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  1,  1,  1,  1,
		1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  1,  1,  1,  1,
	};
	return gpu_command_length[command >> 24];
}

void gpu_write_gp0(uint32_t address, uint32_t value, int bytes) {
	(void)bytes;
	(void)address;

	if (gpu.polyline_mode) {
		if ((value & 0x50005000) == 0x50005000) {
			gpu.polyline_mode = false;
			return;
		}
		Vertex next = read_vertex(value);
		draw_line(gpu.polyline_last_vertex, next, gpu.polyline_last_color, gpu.polyline_last_color, gpu.polyline_semitrans);
		gpu.polyline_last_vertex = next;
		return;
	}

	if (gpu.cpu_to_vram_copy_mode) {
		uint16_t color0 = (uint16_t)(value & 0xFFFF);
		uint16_t color1 = (uint16_t)(value >> 16);
		gpu.vram[gpu.copy_current.y][gpu.copy_current.x] = color0;
		gpu.copy_current.x++;
		if (gpu.copy_current.x == gpu.copy_end.x) {
			gpu.copy_current.x = gpu.copy_start.x;
			gpu.copy_current.y++;
		}
		if (gpu.copy_current.y == gpu.copy_end.y) {
			gpu.cpu_to_vram_copy_mode = false;
			return;
		}
		gpu.vram[gpu.copy_current.y][gpu.copy_current.x] = color1;
		gpu.copy_current.x++;
		if (gpu.copy_current.x == gpu.copy_end.x) {
			gpu.copy_current.x = gpu.copy_start.x;
			gpu.copy_current.y++;
		}
		if (gpu.copy_current.y == gpu.copy_end.y) {
			gpu.cpu_to_vram_copy_mode = false;
			return;
		}
		return;
	}

	if (gpu.remaining_words == 0) {
		gpu.command = (value >> 24);
		uint8_t command_length = gpu_compute_command_length(value);
		gpu.current_word = 0;
		gpu.remaining_words = command_length;
	}
	gpu.word_buffer[gpu.current_word++] = value;
	gpu.remaining_words--;

	if (gpu.remaining_words == 0) {
		gpu_execute_command();
		gpu.command = NO_COMMAND;
		gpu.remaining_words = 0;
		gpu.current_word = 0;
	}
}

void gpu_write_gp1(uint32_t address, uint32_t value, int bytes) {
	(void)address;
	(void)bytes;
	switch (value >> 24) {
	case 0x05:
		gpu.display_area.pos.x = (value >>  0) & 0x3FF;
		gpu.display_area.pos.y = (value >> 10) & 0x1FF;
		break;
	case 0x08:
		if (value & 0x40) {
			gpu.display_area.size.x = 368;
		} else {
			static const int16_t widths[4] = { 256, 320, 512, 640 };
			gpu.display_area.size.x = widths[value & 0x03];
		}

		if (value & 0x04) {
			gpu.display_area.size.y = 480;
		} else {
			gpu.display_area.size.y = 240;
		}
		break;
	default:
		fprintf(stderr, "[INFO] Unimplemented GPU1 write with value %08X\n", value);
	}
}

static inline int16_t find_max(int16_t a, int16_t b, int16_t c) {
	int16_t max = a;
	if (b > max) max = b;
	if (c > max) max = c;
	return max;
}

static inline int16_t find_min(int16_t a, int16_t b, int16_t c) {
	int16_t min = a;
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

static inline void gpu_put_pixel(uint16_t *pixel, uint32_t color, bool semitrans) {
	if (semitrans) {
		*pixel = blend_average16(color32to16(color), *pixel);
	} else {
		*pixel = color32to16(color);
	}
}

static inline void clamp_to_drawing_area(Vertex *v) {
	if      (v->x < gpu.drawing_area_beg.x) v->x = gpu.drawing_area_beg.x;
	else if (v->x > gpu.drawing_area_end.x) v->x = gpu.drawing_area_end.x;

	if      (v->y < gpu.drawing_area_beg.y) v->y = gpu.drawing_area_beg.y;
	else if (v->y > gpu.drawing_area_end.y) v->y = gpu.drawing_area_end.y;
}

void draw_monochrome_rectangle(uint32_t color, bool semitrans, Vertex pos, Vertex size) {
	// Clamp to the dimensions of the canvas
	Vertex end = (Vertex){ .x = pos.x + size.x, .y = pos.y + size.y };
	clamp_to_drawing_area(&pos);
	clamp_to_drawing_area(&end);

	for (int y = pos.y; y < end.y; y++)
	for (int x = pos.x; x < end.x; x++)
		gpu_put_pixel(&gpu.vram[y][x], color, semitrans);
}

static uint32_t apply_dither32(int16_t x, int16_t y, uint32_t color) {
	static const int16_t dither_pattern[4][4] = {
		{ -4, +0, -3, +1 },
		{ +2, -2, +3, -1 },
		{ -3, +1, -4, +0 },
		{ +3, -1, +2, -2 },
	};
	int16_t dither_offset = dither_pattern[y & 0x03][x & 0x03];
	int32_t r = (color >>  0) & 0xFF;
	int32_t g = (color >>  8) & 0xFF;
	int32_t b = (color >> 16) & 0xFF;

	r += dither_offset;
	g += dither_offset;
	b += dither_offset;

	if (r < 0) r = 0;
	if (g < 0) g = 0;
	if (b < 0) b = 0;
	if (r > 255) r = 255;
	if (g > 255) g = 255;
	if (b > 255) b = 255;

	return ((uint32_t)b << 16) | ((uint32_t)g << 8) | (uint32_t)r;
}

static inline void draw_line(Vertex v1, Vertex v2, uint32_t c1, uint32_t c2, bool semitrans) {
	// Degenerate case
	if (v1.x == v2.x && v1.y == v2.y)
		return;

	// Vertical line case
	if (v1.x == v2.x) {
		// Swap so that v1.y is always less than v2.y
		if (v2.y < v1.y) {
			uint32_t temp_c = c1;
			c1 = c2;
			c2 = temp_c;
			Vertex temp_v = v1;
			v1 = v2;
			v2 = temp_v;
		}

		for (int16_t y = v1.y; y <= v2.y; y++) {
			float a1 = (float)y / (float)(v2.y - v1.y);
			uint32_t c = blend_line32(c1, c2, a1, 1.0f - a1);
			gpu.vram[y][v1.x] = color32to16(c);
		}
		return;
	}

	float slope = (float)(v2.y - v1.y) / (float)(v2.x - v1.x);
	float intercept = (float)v1.y - slope * (float)v1.x;
	if (slope <= 1 && slope >= -1) {
		// Swap so that v1.x is always less than v2.x
		if (v2.x < v1.x) {
			uint32_t temp_c = c1;
			c1 = c2;
			c2 = temp_c;
			Vertex temp_v = v1;
			v1 = v2;
			v2 = temp_v;
		}
		for (int16_t x = v1.x; x <= v2.x; x++) {
			int16_t y = (int16_t)roundf(slope * x + intercept);
			float a1 = (float)x / (float)(v2.x - v1.x);
			uint32_t color = blend_line32(c1, c2, a1, 1.0f - a1);
			gpu_put_pixel(&gpu.vram[y][x], color, semitrans);
		}
	} else {
		// Swap so that v1.y is always less than v2.y
		if (v2.y < v1.y) {
			uint32_t temp_c = c1;
			c1 = c2;
			c2 = temp_c;
			Vertex temp_v = v1;
			v1 = v2;
			v2 = temp_v;
		}
		for (int16_t y = v1.y; y <= v2.y; y++) {
			int16_t x = (int16_t)roundf((y - intercept) / slope);
			float a1 = (float)y / (float)(v2.y - v1.y);
			uint32_t color = blend_line32(c1, c2, a1, 1.0f - a1);
			gpu_put_pixel(&gpu.vram[y][x], color, semitrans);
		}
	}
}

static inline void draw_triangle(Triangle triangle, bool shaded, bool semitrans) {
	Vertex v0 = triangle.v0;
	Vertex v1 = triangle.v1;
	Vertex v2 = triangle.v2;
	uint32_t c0 = triangle.c0;
	uint32_t c1 = triangle.c1;
	uint32_t c2 = triangle.c2;

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
		uint32_t c_temp = c0;
		c0 = c1;
		c1 = c_temp;
		double_area *= -1;
	}

	// Compute the bounding box of the triangle
	Vertex start = {
		.x = find_min(v0.x, v1.x, v2.x),
		.y = find_min(v0.y, v1.y, v2.y),
	};
	Vertex end = {
		.x = find_max(v0.x, v1.x, v2.x),
		.y = find_max(v0.y, v1.y, v2.y),
	};

	// Clamp to the dimensions of the canvas
	clamp_to_drawing_area(&start);
	clamp_to_drawing_area(&end);

	// Compute the w0 starts and the x/y deltas
	int w0_start = vec2_cross(
		vec2_sub(v0, start),
		vec2_sub(v1, start)
		);
	int w0_delta_x = v0.y - v1.y;
	int w0_delta_y = v1.x - v0.x;

	// Compute the w1 starts and the x/y deltas
	int w1_start = vec2_cross(
		vec2_sub(v1, start),
		vec2_sub(v2, start)
		);
	int w1_delta_x = v1.y - v2.y;
	int w1_delta_y = v2.x - v1.x;

	// Compute the w2 starts and the x/y deltas
	int w2_start = vec2_cross(
		vec2_sub(v2, start),
		vec2_sub(v0, start)
		);
	int w2_delta_x = v2.y - v0.y;
	int w2_delta_y = v0.x - v2.x;

	// Compute the edge biases to prevent overlap
	if (is_top_or_left(v0, v1)) w0_start += 1;
	if (is_top_or_left(v1, v2)) w1_start += 1;
	if (is_top_or_left(v2, v0)) w2_start += 1;

	uint16_t *pixel_start = &gpu.vram[start.y][start.x];
	int pixel_delta_y = VRAM_WIDTH;

	bool reached_triangle = false;

	for (int16_t y = start.y; y <= end.y; y++) {
		int w0 = w0_start;
		int w1 = w1_start;
		int w2 = w2_start;
		uint16_t *pixel = pixel_start;
		for (int16_t x = start.x; x <= end.x; x++) {
			bool inside_triangle = w0 > 0 && w1 > 0 && w2 > 0;
			if (inside_triangle) {
				reached_triangle = true;
				uint32_t color;

				if (shaded) {
					float a0 = (float)w1 / (float)double_area;
					float a1 = (float)w2 / (float)double_area;
					float a2 = (float)w0 / (float)double_area;
					color = blend_triangle32(c0, c1, c2, a0, a1, a2);
					if (gpu.dithering_enabled)
						color = apply_dither32(x, y, color);
				} else {
					color = c0;
				}
				gpu_put_pixel(pixel, color, semitrans);
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
