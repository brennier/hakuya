#include "gpu.h"
#include <stdint.h>

int8_t   vram[512][2048] = { 0 };
uint32_t output[512][1024] = { 0 };

static inline uint32_t convert_color16(uint16_t c) {
	uint32_t result =
		((c << 9) & 0x00F80000) |
		((c << 6) & 0x0000F800) |
		((c << 3) & 0x000000F8);
	result |= (result >> 5) & 0x00070707;
	return result | 0xFF000000;
}

const uint32_t *render_vram(void) {
	uint8_t  *flat_vram   = (uint8_t  *)vram;
	uint32_t *flat_output = (uint32_t *)output;
	for (int i = 0; i < 512 * 1024; i++) {
		uint16_t pixel = (uint16_t)((flat_vram[2*i + 1] << 8) | flat_vram[2*i]);
		flat_output[i] = convert_color16(pixel);
	}
	return flat_output;
}
