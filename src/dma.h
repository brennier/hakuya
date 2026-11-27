#ifndef HAKUYA_DMA_H
#define HAKUYA_DMA_H

#include <stdint.h>

struct HakuyaCore;

struct DMAChannel {
	uint32_t start;
	uint32_t blocks;
	uint32_t control;
};

enum DMAChannelName {
	DMA_MDEC_IN = 0,
	DMA_MDEC_OUT,
	DMA_GPU,
	DMA_CDROM,
	DMA_SPU,
	DMA_PIO,
	DMA_OTC,
};

struct HakuyaDMA {
	struct DMAChannel channel[7]; // indexed by channel number
	uint32_t control;
	uint32_t interrupt;
};

void dma_init(struct HakuyaCore *core);
uint32_t dma_read(struct HakuyaCore *core, uint32_t address, int bytes);
void dma_write(struct HakuyaCore *core, uint32_t address, uint32_t value, int bytes);

#endif // HAGKUYA_DMA_H
