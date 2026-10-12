#include "dma.h"
#include "bus.h"
#include <stdio.h>
#include <stdbool.h>

#define GPU0_ADDRESS 0x1F801810

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

static inline void dma_trigger_otc(void) {
	uint32_t address    = dma.otc.start & 0x00FFFFFCu;
	uint32_t word_count = dma.otc.block_control & 0xFFFFu;
	printf("DMA OTC triggered: start %08X, size %08X\n",
	       address, word_count);

	unsigned i = 0;
	while (i < word_count) {
		bus_write32(address, address - 4);
		address -= 4;
		i++;
	}
	bus_write32(address + 4, 0x00FFFFFF);
}

static inline void dma_trigger_gpu(void) {
	uint32_t address = dma.gpu.start & 0x00FFFFFCu;
	uint32_t sync_mode = (dma.gpu.channel_control >> 9) & 0x03;
	printf("DMA GPU triggered: start %08X mode %08X\n", address, sync_mode);
	if (sync_mode != 2) {
		printf("Implement mode %d for GPU DMA!", sync_mode);
		return;
	}
	while (true) {
		uint32_t header = bus_read32(address);
		uint32_t word_count = header >> 24;
		for (int i = 0; i < word_count; i++) {
			uint32_t data = bus_read32(address + 4*(i+1));
			printf("Sending %08X\n", data);
			bus_write32(GPU0_ADDRESS, data);
		}

		address = header & 0x00FFFFFF;
		if (address & (1u << 23)) {
			break;
		}
	}
}

void dma_write(uint32_t relative_address, uint32_t value, int bytes) {
	(void)bytes;
	if (bytes != 4) {
		printf("Tried to write %d bytes\n", bytes);
	}
	switch (relative_address >> 2) {
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
	case 10: dma.gpu.channel_control = value;
		if (value & (1u << 24)) {
			dma_trigger_gpu();
		}
		break;
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
	case 26: dma.otc.channel_control = value;
		if (value & (1u << 28)) {
			dma_trigger_otc();
		}
		break;
	case 27: break;

	// General DMA settings
	case 28: dma.control   = value; break;
	case 29: dma.interrupt = value; break;
	}
}
