#include "dma.h"

enum SyncMode {
	SYNC_IMMEDIATE   = 0,
	SYNC_BLOCKS      = 1,
	SYNC_LINKED_LIST = 2,
	SYNC_UNUSED      = 3,
};

struct DMAChannel {
	uint32_t start;
	uint32_t block_control;
	uint32_t channel_control;
};

struct DMA {
	struct DMAChannel mdec_in;
	struct DMAChannel mdec_out;
	struct DMAChannel gpu;
	struct DMAChannel cdrom;
	struct DMAChannel spu;
	struct DMAChannel pio;
	struct DMAChannel otc;
	uint32_t control;
	uint32_t interrupt;
};

struct DMA dma = {
	.control = (uint32_t)0x07654321,
};

void dma_register_write(uint32_t relative_address, uint32_t value) {
	switch (relative_address / 4) {
	// Channel 0 (MDEC_IN)
	case 0: dma.mdec_in.start           = value; break;
	case 1: dma.mdec_in.block_control   = value; break;
	case 2: dma.mdec_in.channel_control = value; break;
	case 3: break;

	// Channel 1 (MDEC_OUT)
	case 4: dma.mdec_out.start           = value; break;
	case 5: dma.mdec_out.block_control   = value; break;
	case 6: dma.mdec_out.channel_control = value; break;
	case 7: break;

	// Channel 2 (GPU)
	case 8:  dma.gpu.start           = value; break;
	case 9:  dma.gpu.block_control   = value; break;
	case 10: dma.gpu.channel_control = value; break;
	case 11: break;

	// Channel 3 (CDROM)
	case 12: dma.cdrom.start           = value; break;
	case 13: dma.cdrom.block_control   = value; break;
	case 14: dma.cdrom.channel_control = value; break;
	case 15: break;

	// Channel 4 (SPU)
	case 16: dma.spu.start           = value; break;
	case 17: dma.spu.block_control   = value; break;
	case 18: dma.spu.channel_control = value; break;
	case 19: break;

	// Channel 5 (PIO)
	case 20: dma.pio.start           = value; break;
	case 21: dma.pio.block_control   = value; break;
	case 22: dma.pio.channel_control = value; break;
	case 23: break;

	// Channel 6 (OTC)
	case 24: dma.otc.start           = value; break;
	case 25: dma.otc.block_control   = value; break;
	case 26: dma.otc.channel_control = value; break;
	case 27: break;

	// General DMA settings
	case 28: dma.control   = value; break;
	case 29: dma.interrupt = value; break;
	}
}
