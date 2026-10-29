#ifndef HAKUYA_GPU_H
#define HAKUYA_GPU_H

#include <stdint.h>
#include <stdbool.h>

#define VRAM_WIDTH  1024
#define VRAM_HEIGHT 512

struct HakuyaCore;

typedef struct {
	uint8_t r;
	uint8_t g;
	uint8_t b;
} Color24;

typedef struct {
	int16_t x;
	int16_t y;
} Vertex;

enum BlendMode {
	BLEND_AVERAGE  = 0, // (B + F)/2
	BLEND_ADD      = 1, // B + F
	BLEND_SUBTRACT = 2, // B - F
	BLEND_QUARTER  = 3, // B + F/4
	BLEND_NONE,
};

typedef struct {
	Color24  color[3];
	Vertex   vertex[3];
	Vertex   uv[3];
	Vertex   clut;
	Vertex   texpage;
	Vertex   drawing_area_start, drawing_area_end;
	uint8_t  texture_bpp;
	uint8_t  blend_mode;
	bool     shaded, textured, raw_texture, dithering;
	bool     mask_bit_set, mask_bit_check;
} TrianglePrimitive;

typedef struct {
	Color24  color;
	Vertex   start;
	Vertex   size;
	Vertex   uv;
	Vertex   texpage;
	Vertex   clut;
	Vertex   drawing_area_start, drawing_area_end;
	uint8_t  texture_bpp;
	uint8_t  blend_mode;
	bool     x_flip, y_flip, textured, raw_texture;
	bool     mask_bit_set, mask_bit_check;
} RectanglePrimitive;

typedef struct {
	Color24 color[2];
	Vertex  start;
	Vertex  end;
	Vertex  drawing_area_start, drawing_area_end;
	uint8_t blend_mode;
	bool    shaded, dithering;
	bool    mask_bit_set, mask_bit_check;
} LinePrimitive;

struct HakuyaGPU {
	uint32_t word_buffer[12];
	uint8_t  current_word;
	uint8_t  remaining_words;

	LinePrimitive polyline;
	bool polyline_active;
	bool polyline_unfinished;

	Vertex display_area_start;
	Vertex display_area_size;
	Vertex drawing_area_start;
	Vertex drawing_area_end;
	bool color_mode_24bit;
	bool display_enabled;

	bool mask_bit_set;
	bool mask_bit_check;

	uint32_t draw_mode_settings;
	bool dithering_enabled;

	bool cpu_to_vram_copy_mode;
	Vertex copy_current;
	Vertex copy_start;
	Vertex copy_end;

	uint16_t vram[VRAM_HEIGHT][VRAM_WIDTH];
};

uint32_t gpu_read_gp0(struct HakuyaCore *core, uint32_t address, int bytes);
uint32_t gpu_read_gp1(struct HakuyaCore *core, uint32_t address, int bytes);
void gpu_write_gp0(struct HakuyaCore *core, uint32_t address, uint32_t value, int bytes);
void gpu_write_gp1(struct HakuyaCore *core, uint32_t address, uint32_t value, int bytes);

void gpu_render_vram(struct HakuyaCore *core, uint16_t *output_buffer);
void gpu_render_frame(struct HakuyaCore *core, uint32_t *output_buffer, int *width, int *height);

#endif // HAGKUYA_GPU_H
