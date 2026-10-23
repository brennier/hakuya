#ifndef HAKUYA_DMA_H
#define HAKUYA_DMA_H

#include <stdint.h>

struct HakuyaCore;

struct HakuyaDMA {
	uint32_t registers[32];
};

void dma_init(struct HakuyaCore *core);
uint32_t dma_read(struct HakuyaCore *core, uint32_t address, int bytes);
void dma_write(struct HakuyaCore *core, uint32_t address, uint32_t value, int bytes);

#endif // HAGKUYA_DMA_H
