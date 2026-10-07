#ifndef HAKUYA_GPU_H
#define HAKUYA_GPU_H

#include <stdint.h>

uint32_t gpu_read(uint32_t address, int bytes);
void gpu_write(uint32_t address, uint32_t value, int bytes);

const uint32_t *render_vram(void);

#endif // HAGKUYA_GPU_H
