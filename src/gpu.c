#include "gpu.h"
#include <stdio.h>
#include <stdbool.h>
#include <string.h>

#define VRAM_WIDTH  1024
#define VRAM_HEIGHT 512

enum MiscGPU0Command {
	NO_OP                 = 0x00,
	FLUSH_TEXTURE_CACHE   = 0x01,
	FILL_RECTANGLE        = 0x02,
	SET_GPU_IRQ_FLAG      = 0x1F,
	SET_DRAWING_SETTINGS  = 0xE1,
	SET_DRAWING_MASK      = 0xE2,
	SET_DRAWING_AREA_BEG  = 0xE3,
	SET_DRAWING_AREA_END  = 0xE4,
	SET_DRAWING_OFFSET    = 0xE5,
	SET_MASK_BIT_SETTINGS = 0xE6,
};

typedef struct {
	uint8_t r;
	uint8_t g;
	uint8_t b;
} Color24;

typedef struct {
	int16_t x;
	int16_t y;
} Vertex;

typedef struct {
	Vertex pos;
	Vertex size;
} Rectangle;

typedef struct {
	enum {
		NONE,
		POLY,
		QUAD,
		LINE,
		POLYLINE,
		RECT,
		FILL,
	} type;
	Vertex vertex[4];
	Vertex uv[4];
	Vertex clut;
	Vertex size;
	Vertex texpage;
	Color24 color[4];
	uint16_t texpage_raw;
	bool shaded, textured, semitrans, raw_texture;
	bool unfinished_polyline;
} DrawingPrimitive;

struct HakuyaGPU {
	DrawingPrimitive current_drawing;
	uint32_t word_buffer[12];
	uint8_t  current_word;
	uint8_t  remaining_words;
	Rectangle display_area;
	Vertex drawing_area_beg;
	Vertex drawing_area_end;

	uint32_t draw_mode_settings;
	bool dithering_enabled;

	bool cpu_to_vram_copy_mode;
	Vertex copy_current;
	Vertex copy_start;
	Vertex copy_end;

	uint16_t vram[VRAM_HEIGHT][VRAM_WIDTH];
} gpu;

static inline Color24 blend_triangle24(Color24 c1, Color24 c2, Color24 c3,
				       float a1, float a2, float a3) {
	Color24 result;
	result.r = (uint8_t)(c1.r * a1 + c2.r * a2 + c3.r * a3);
	result.g = (uint8_t)(c1.g * a1 + c2.g * a2 + c3.g * a3);
	result.b = (uint8_t)(c1.b * a1 + c2.b * a2 + c3.b * a3);
	return result;
}

static inline Color24 interpolate_color24(Color24 c1, Color24 c2, float weight) {
	float a2 = weight;
	float a1 = 1.0f - weight;
	Color24 result;
	result.r = (uint8_t)(c1.r * a1 + c2.r * a2);
	result.g = (uint8_t)(c1.g * a1 + c2.g * a2);
	result.b = (uint8_t)(c1.b * a1 + c2.b * a2);
	return result;
}

static inline uint16_t color24to16(Color24 c) {
	c.r >>= 3;
	c.g >>= 3;
	c.b >>= 3;
	return (c.b << 10) | (c.g << 5) | c.r;
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

static inline Color24 read_color(uint32_t word) {
	return (Color24){
		.r = ((word >>  0) & 0xFF),
		.g = ((word >>  8) & 0xFF),
		.b = ((word >> 16) & 0xFF),
	};
}

static inline Vertex read_texpage(uint32_t word) {
	return (Vertex){
		.x = (word & 0x0F) << 6,
		.y = (word & 0x10) << 4,
	};
}

static inline Vertex read_texcoord(uint32_t word) {
	return (Vertex){
		.x = (word >> 0) & 0xFF,
		.y = (word >> 8) & 0xFF,
	};
}

static inline Vertex read_clut(uint32_t word) {
	word >>= 16;
	return (Vertex){
		.x = (word & 0x3F) << 4,
		.y = (word >> 6) & 0x1FF,
	};
}

static inline void draw_rectangle(DrawingPrimitive rectangle);
static inline void draw_triangle(DrawingPrimitive triangle);
static inline void draw_line(DrawingPrimitive line);

static DrawingPrimitive gpu_parse_polygon_command(uint32_t command, uint32_t *arguments) {
	DrawingPrimitive polygon = {
		.type      = ((command >> 27) & 1u) ? QUAD : POLY,
		.shaded    = (command >> 28) & 1u,
		.textured  = (command >> 26) & 1u,
		.semitrans = (command >> 25) & 1u,
		.raw_texture = (command >> 24) & 1u,
	};
	int j = 0;
	for (int i = 0; i < ((polygon.type == QUAD) ? 4 : 3); i++) {
		if (polygon.shaded) {
			polygon.color[i] = read_color(arguments[j++]);
		} else if (i == 0) {
			polygon.color[i] = read_color(arguments[0]);
			j++;
		} else {
			polygon.color[i] = read_color(arguments[0]);
		}
		polygon.vertex[i] = read_vertex(arguments[j++]);
		if (polygon.textured) {
			polygon.uv[i] = read_texcoord(arguments[j]);
			if (i == 0) {
				polygon.clut = read_clut(arguments[j]);
			} else if (i == 1) {
				polygon.texpage = read_texpage(arguments[j] >> 16);
				polygon.texpage_raw = (arguments[j] >> 16);
			}
			j++;
		}
	}
	return polygon;
}

static DrawingPrimitive gpu_parse_line_command(uint32_t command, uint32_t *arguments) {
	DrawingPrimitive line = {
		.type      = ((command >> 27) & 1u) ? POLYLINE : LINE,
		.color     = { read_color(arguments[0]) },
		.vertex    = { read_vertex(arguments[1]) },
		.shaded    = (command >> 28) & 1u,
		.semitrans = (command >> 25) & 1u,
	};
	if (line.shaded) {
		line.color[1]  = read_color(arguments[2]);
		line.vertex[1] = read_vertex(arguments[3]);
	} else {
		line.color[1]  = line.color[0];
		line.vertex[1] = read_vertex(arguments[2]);
	}
	return line;
}

static DrawingPrimitive gpu_parse_rectangle_command(uint32_t command, uint32_t *arguments) {
	static const Vertex rectangle_size[4] = {
		{ .x = 0,  .y = 0  },
		{ .x = 1,  .y = 1  },
		{ .x = 8,  .y = 8  },
		{ .x = 16, .y = 16 },
	};
	DrawingPrimitive rectangle = {
		.type      = RECT,
		.color     = { read_color(arguments[0]) },
		.vertex    = { read_vertex(arguments[1]) },
		.textured  = (command >> 26) & 1u,
		.semitrans = (command >> 25) & 1u,
		.raw_texture = (command >> 24) & 1u,
		.size = rectangle_size[(command >> 27) & 3u],
	};
	int i = 2;
	if (rectangle.textured) {
		rectangle.uv[0]   = read_texcoord(arguments[i]);
		rectangle.clut    = read_clut(arguments[i]);
		rectangle.texpage = read_texpage(gpu.draw_mode_settings);
		rectangle.texpage_raw = gpu.draw_mode_settings & 0xFFFF;
		i++;
	}
	if (rectangle.size.x == 0) {
		rectangle.size = read_vertex(arguments[i++]);
	}
	return rectangle;
}

static void gpu_execute_command(uint32_t command) {
	switch (command >> 29) {
	case 0:
		break; // Misc commands (handled later)
	case 1: { // Draw polygon
		DrawingPrimitive polygon = gpu_parse_polygon_command(command, gpu.word_buffer);
		draw_triangle(polygon);
		if (polygon.type == QUAD) {
			polygon.color[0] = polygon.color[1];
			polygon.color[1] = polygon.color[2];
			polygon.color[2] = polygon.color[3];
			polygon.vertex[0] = polygon.vertex[1];
			polygon.vertex[1] = polygon.vertex[2];
			polygon.vertex[2] = polygon.vertex[3];
			polygon.uv[0] = polygon.uv[1];
			polygon.uv[1] = polygon.uv[2];
			polygon.uv[2] = polygon.uv[3];
			draw_triangle(polygon);
		}
		return;
	}
	case 2: { // Draw line
		DrawingPrimitive line = gpu_parse_line_command(command, gpu.word_buffer);
		if (line.type == POLYLINE) {
			gpu.current_drawing = line;
		}
		draw_line(line);
		return;
	}
	case 3: { // Draw Rectangle
		DrawingPrimitive rectangle = gpu_parse_rectangle_command(command, gpu.word_buffer);
		draw_rectangle(rectangle);
		return;
	}
	case 4: { // VRAM-to-VRAM copy
		Vertex src  = read_vertex(gpu.word_buffer[1]);
		Vertex dest = read_vertex(gpu.word_buffer[2]);
		Vertex size = read_vertex(gpu.word_buffer[3]);
		if (size.x == 0) size.x = 0x400;
		if (size.y == 0) size.y = 0x200;
		for (int y = 0; y < size.y; y++)
			for (int x = 0; x < size.x; x++)
				gpu.vram[dest.y + y][dest.x + x] = gpu.vram[src.y + y][src.x + x];
		return;
	}
	case 5: { // CPU-to-VRAM copy
		gpu.cpu_to_vram_copy_mode = true;
		gpu.copy_start = read_vertex(gpu.word_buffer[1]);
		gpu.copy_current = gpu.copy_start;
		Vertex size = read_vertex(gpu.word_buffer[2]);
		if (size.x == 0) size.x = 0x400;
		if (size.y == 0) size.y = 0x200;
		gpu.copy_end = (Vertex){ gpu.copy_start.x + size.x, gpu.copy_start.y + size.y };
		return;
	}
	case 6: { // VRAM-to-CPU copy
		return;
	}
	case 7: // Environment commands (handled later)
		break;
	}

	enum MiscGPU0Command misc_command = (command >> 24);
	switch (misc_command) {
	case NO_OP: break;
	case FILL_RECTANGLE: {
		uint16_t color16 = color24to16(read_color(gpu.word_buffer[0]));
		Vertex pos = read_vertex(gpu.word_buffer[1]);
		pos.x &= 0x03F0;
		Vertex size = read_vertex(gpu.word_buffer[2]);
		size.x += 0x0F;
		size.x &= ~0x0F;
		for (int y = 0; y < size.y; y++)
		for (int x = 0; x < size.x; x++)
			gpu.vram[pos.y + y][pos.x + x] = color16;
		break;
	}
	case SET_DRAWING_SETTINGS:
		gpu.draw_mode_settings = gpu.word_buffer[0];
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

	if (gpu.current_drawing.type == POLYLINE) {
		if (gpu.current_drawing.unfinished_polyline) {
			gpu.current_drawing.vertex[0] = gpu.current_drawing.vertex[1];
			gpu.current_drawing.vertex[1] = read_vertex(value);
			gpu.current_drawing.unfinished_polyline = false;
			draw_line(gpu.current_drawing);
		} else if ((value & 0x50005000) == 0x50005000) {
			gpu.current_drawing = (DrawingPrimitive){ 0 };
		} else if (gpu.current_drawing.shaded) {
			gpu.current_drawing.color[0] = gpu.current_drawing.color[1];
			gpu.current_drawing.color[1] = read_color(value);
			gpu.current_drawing.unfinished_polyline = true;
		} else {
			gpu.current_drawing.vertex[0] = gpu.current_drawing.vertex[1];
			gpu.current_drawing.vertex[1] = read_vertex(value);
			draw_line(gpu.current_drawing);
		}
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
		uint8_t command_length = gpu_compute_command_length(value);
		gpu.current_word = 0;
		gpu.remaining_words = command_length;
	}
	gpu.word_buffer[gpu.current_word++] = value;
	gpu.remaining_words--;

	if (gpu.remaining_words == 0) {
		gpu_execute_command(gpu.word_buffer[0]);
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

static inline void gpu_put_pixel(uint16_t *pixel, Color24 color, bool semitrans) {
	if (semitrans) {
		*pixel = blend_average16(color24to16(color), *pixel);
	} else {
		*pixel = color24to16(color);
	}
}

static inline void clamp_to_drawing_area(Vertex *v) {
	if      (v->x < gpu.drawing_area_beg.x) v->x = gpu.drawing_area_beg.x;
	else if (v->x > gpu.drawing_area_end.x) v->x = gpu.drawing_area_end.x;

	if      (v->y < gpu.drawing_area_beg.y) v->y = gpu.drawing_area_beg.y;
	else if (v->y > gpu.drawing_area_end.y) v->y = gpu.drawing_area_end.y;
}

static inline uint16_t blend_texture(uint16_t tex_color, Color24 shade_color) {
	uint8_t tex_r = (tex_color >>  0) & 0x1Fu;
	uint8_t tex_g = (tex_color >>  5) & 0x1Fu;
	uint8_t tex_b = (tex_color >> 10) & 0x1Fu;
	uint32_t r = (uint32_t)(tex_r * shade_color.r) >> 7;
	uint32_t g = (uint32_t)(tex_g * shade_color.g) >> 7;
	uint32_t b = (uint32_t)(tex_b * shade_color.b) >> 7;

	r = (r > 31) ? 31 : r;
	g = (g > 31) ? 31 : g;
	b = (b > 31) ? 31 : b;
	return (uint16_t)((tex_color & 0x8000u) | (b << 10) | (g << 5) | (r << 0));
}

static inline uint16_t get_texel(Vertex texpage, uint8_t u, uint8_t v, uint8_t bpp, const uint16_t *clut) {
	switch (bpp) {
	case 0: {
		uint16_t index = gpu.vram[texpage.y + v][texpage.x + (u >> 2)];
		index >>= 4 * (u % 4);
		index &= 0x0F;
		return clut[index];
	}
	case 1: {
		uint16_t index = gpu.vram[texpage.y + v][texpage.x + (u >> 1)];
		index >>= 8 * (u % 2);
		index &= 0xFF;
		return clut[index];
	}
	case 2: {
		return gpu.vram[texpage.y + v][texpage.x + u];
	}
	default: {
		fprintf(stderr, "[WARNING] Unknown bpp parameter: %d\n", bpp);
		return gpu.vram[texpage.y + v][texpage.x + u];
	}
	}
}


static inline void draw_rectangle(DrawingPrimitive rectangle) {
	// Clamp to the dimensions of the canvas
	Color24 color = rectangle.color[0];
	bool semitrans = rectangle.semitrans;
	Vertex pos  = rectangle.vertex[0];
	Vertex size = rectangle.size;
	Vertex end = (Vertex){
		.x = pos.x + size.x,
		.y = pos.y + size.y,
	};
	clamp_to_drawing_area(&pos);
	clamp_to_drawing_area(&end);

	if (!rectangle.textured) {
		for (int y = pos.y; y < end.y; y++)
		for (int x = pos.x; x < end.x; x++)
			gpu_put_pixel(&gpu.vram[y][x], color, semitrans);
		return;
	}

	Vertex texpage = rectangle.texpage;
	bool x_flip = (rectangle.texpage_raw >> 12) & 1u;
	bool y_flip = (rectangle.texpage_raw >> 13) & 1u;
	uint8_t bpp = (rectangle.texpage_raw >> 7) & 3u;
	uint16_t clut[256];
	switch (bpp) {
	case 0: memcpy(clut, &gpu.vram[rectangle.clut.y][rectangle.clut.x], 32);  break;
	case 1: memcpy(clut, &gpu.vram[rectangle.clut.y][rectangle.clut.x], 512); break;
	}

	uint8_t u0 = (uint8_t)rectangle.uv[0].x;
	uint8_t u = u0;
	uint8_t v = (uint8_t)rectangle.uv[0].y;
	uint8_t u_dir = x_flip ? (uint8_t)-1 : 1;
	uint8_t v_dir = y_flip ? (uint8_t)-1 : 1;

	for (int y = 0; y < size.y; y++, v += v_dir) {
		u = u0;
		for (int x = 0; x < size.x; x++, u += u_dir) {
			uint16_t tex_color = get_texel(texpage, u, v, bpp, clut);
			if (!(tex_color & 0x8000u)) {
				continue;
			}
			if (!rectangle.raw_texture) {
				tex_color = blend_texture(tex_color, color);
			}
			uint16_t *pixel = &gpu.vram[pos.y + y][pos.x + x];
			if (semitrans) {
				*pixel = blend_average16(tex_color, *pixel);
			} else {
				*pixel = tex_color;
			}
		}
	}
}

static Color24 apply_dither24(int16_t x, int16_t y, Color24 color) {
	static const int8_t dither_pattern[4][4] = {
		{ -4, +0, -3, +1 },
		{ +2, -2, +3, -1 },
		{ -3, +1, -4, +0 },
		{ +3, -1, +2, -2 },
	};
	int8_t dither_offset = dither_pattern[y & 0x03][x & 0x03];
	int32_t r = color.r + dither_offset;
	int32_t g = color.g + dither_offset;
	int32_t b = color.b + dither_offset;

	if (r < 0) r = 0;
	if (g < 0) g = 0;
	if (b < 0) b = 0;
	if (r > 255) r = 255;
	if (g > 255) g = 255;
	if (b > 255) b = 255;

	return (Color24){ (uint8_t)r, (uint8_t)g, (uint8_t)b };
}

static inline int16_t abs16(int16_t x) {
	return (x < 0) ? (int16_t)-x : x;
}

static inline void draw_line(DrawingPrimitive line) {
	Vertex  v0 = line.vertex[0];
	Vertex  v1 = line.vertex[1];
	Color24 c0 = line.color[0];
	Color24 c1 = line.color[1];
	bool semitrans = line.semitrans;
	bool dithering = gpu.dithering_enabled;

	Vertex delta = { .x = v1.x - v0.x, .y = v1.y - v0.y };
	int16_t step = (abs16(delta.x) > abs16(delta.y)) ? abs16(delta.x) : abs16(delta.y);

	// Degenerate case
	if (step == 0) {
		if (dithering) c0 = apply_dither24(v0.x, v0.y, c0);
		gpu_put_pixel(&gpu.vram[v0.y][v0.x], c0, semitrans);
		return;
	}

	float step_x = (float)delta.x / (float)step;
	float step_y = (float)delta.y / (float)step;
	float step_c = 1.0f / (float)step;
	for (int i = 0; i <= step; i++) {
		int16_t x = (int16_t)((float)v0.x + (float)i * step_x + 0.5f);
		int16_t y = (int16_t)((float)v0.y + (float)i * step_y + 0.5f);
		Color24 c = interpolate_color24(c0, c1, (float)i * step_c);
		if (dithering) c = apply_dither24(x, y, c);
		gpu_put_pixel(&gpu.vram[y][x], c, semitrans);
	}
}

static inline uint16_t compute_tritexel(const DrawingPrimitive *triangle, float a0, float a1, float a2) {
	uint8_t bpp = (triangle->texpage_raw >> 7) & 3u;
	Vertex uv0 = triangle->uv[0];
	Vertex uv1 = triangle->uv[1];
	Vertex uv2 = triangle->uv[2];
	int u = uv0.x * a0 + uv1.x * a1 + uv2.x * a2;
	int v = uv0.y * a0 + uv1.y * a1 + uv2.y * a2;
	u &= 0xFF;
	v &= 0xFF;
	const uint16_t *clut = &gpu.vram[triangle->clut.y][triangle->clut.x];
	return get_texel(triangle->texpage, (uint8_t)u, (uint8_t)v, bpp, clut);
}

static inline void draw_triangle(DrawingPrimitive triangle) {
	Vertex v0 = triangle.vertex[0];
	Vertex v1 = triangle.vertex[1];
	Vertex v2 = triangle.vertex[2];
	Color24 c0 = triangle.color[0];
	Color24 c1 = triangle.color[1];
	Color24 c2 = triangle.color[2];
	bool shaded = triangle.shaded;
	bool textured = triangle.textured;
	bool raw_texture = triangle.raw_texture;
	bool semitrans = triangle.semitrans;
	bool dithering = gpu.dithering_enabled && triangle.shaded;
	bool transparent = false;

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
		Color24 c_temp = c0;
		c0 = c1;
		c1 = c_temp;
		double_area *= -1;
		Vertex uv_temp = triangle.uv[0];
		triangle.uv[0] = triangle.uv[1];
		triangle.uv[1] = uv_temp;
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
				float a0 = (float)w1 / (float)double_area;
				float a1 = (float)w2 / (float)double_area;
				float a2 = (float)w0 / (float)double_area;
				Color24 color24 = c0;
				if (shaded && !raw_texture) {
					color24 = blend_triangle24(c0, c1, c2, a0, a1, a2);
					if (dithering)
						color24 = apply_dither24(x, y, color24);
				}
				uint16_t color16;
				if (textured) {
					color16 = compute_tritexel(&triangle, a0, a1, a2);
					if (color16 == 0) {
						transparent = true;
					} else if (!raw_texture) {
						color16 = blend_texture(color16, color24);
					}
				} else {
					color16 = color24to16(color24);
				}
				if (transparent) {
					;
				} else if (semitrans) {
					color16 = blend_average16(color16, *pixel);
					*pixel = color16;
				} else {
					*pixel = color16;
				}
				transparent = false;
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
