#ifndef HAKUYA_GPU_H
#define HAKUYA_GPU_H

#include <stdint.h>

uint32_t gpu_read_gp0(uint32_t address, int bytes);
uint32_t gpu_read_gp1(uint32_t address, int bytes);
void gpu_write_gp0(uint32_t address, uint32_t value, int bytes);
void gpu_write_gp1(uint32_t address, uint32_t value, int bytes);

void gpu_render_vram_u16(uint16_t *output_buffer);
void gpu_render_vram_u32(uint32_t *output_buffer);
void gpu_get_display_area(int *x, int *y, int *width, int *height);

#endif // HAGKUYA_GPU_H
