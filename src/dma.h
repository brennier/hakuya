#ifndef HAKUYA_DMA_H
#define HAKUYA_DMA_H

#include <stdint.h>

uint32_t dma_read(uint32_t address, int bytes);
void dma_write(uint32_t address, uint32_t value, int bytes);

#endif // HAGKUYA_DMA_H
