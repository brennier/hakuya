#ifndef HAKUYA_GPU_H
#define HAKUYA_GPU_H

#include <stdint.h>

uint32_t gpu_read(uint32_t address, int bytes);
void gpu_write(uint32_t address, uint32_t value, int bytes);

void gpu_render_vram_u16(uint16_t *output_buffer);
void gpu_render_vram_u32(uint32_t *output_buffer);

#endif // HAGKUYA_GPU_H
