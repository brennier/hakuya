#include "dma.h"
#include "bus.h"
#include "core.h"
#include <stdio.h>
#include <stdbool.h>
#include <string.h>

#define GPU0_ADDRESS 0x1F801810

enum SyncMode {
	SYNC_IMMEDIATE   = 0,
	SYNC_BLOCKS      = 1,
	SYNC_LINKED_LIST = 2,
	SYNC_UNUSED      = 3,
};

void dma_init(struct HakuyaCore *core) {
	memset(&core->dma, 0, sizeof(struct HakuyaDMA));
	core->dma.control = (uint32_t)0x07654321u;
}

static inline void dma_trigger_otc(struct HakuyaCore *core) {
	uint32_t address    = core->dma.channel[DMA_OTC].start  & 0x00FFFFFCu;
	uint32_t word_count = core->dma.channel[DMA_OTC].blocks & 0x0000FFFFu;
	printf("DMA OTC triggered: start %08X, size %08X\n",
	       address, word_count);

	unsigned i = 0;
	while (i < word_count) {
		bus_write32(core, address, address - 4);
		address -= 4;
		i++;
	}
	bus_write32(core, address + 4, 0x00FFFFFF);
}

static inline void dma_port_write(struct HakuyaCore *core, enum DMAChannelName ch, uint32_t data) {
	switch (ch) {
	case DMA_GPU:
		bus_write32(core, GPU0_ADDRESS, data);
		break;
	default:
		fprintf(stderr, "Error: The channel %d does not have a write port implemented\n", ch);
		break;
	}
}

static inline void dma_trigger(struct HakuyaCore *core, enum DMAChannelName ch) {
	uint32_t address   = core->dma.channel[ch].start & 0x00FFFFFCu; // START
	uint32_t blocks    = core->dma.channel[ch].blocks;              // BLOCKS
	uint32_t control   = core->dma.channel[ch].control;             // CONTROL

	uint32_t sync_mode = (control >> 9) & 0x03;
	printf("DMA Channel %d triggered: start %08X mode %08X\n", ch, address, sync_mode);
	switch (sync_mode) {
	case 0: {
		uint32_t word_count = blocks & 0xFFFFu;
		for (uint32_t i = 0; i < word_count; i++) {
			uint32_t data = bus_read32(core, address + 4*i);
			dma_port_write(core, ch, data);
		}
		return;
	}
	case 1: {
		uint32_t block_size  = blocks & 0xFFFFu;
		uint32_t block_count = blocks >> 16;
		for (uint32_t i = 0; i < block_size * block_count; i++) {
			uint32_t data = bus_read32(core, address + 4*i);
			dma_port_write(core, ch, data);
		}
		return;
	}
	case 2: {
		while (true) {
			uint32_t header = bus_read32(core, address);
			uint32_t word_count = header >> 24;
			for (uint32_t i = 0; i < word_count; i++) {
				uint32_t data = bus_read32(core, address + 4*(i+1));
				printf("Sending %08X\n", data);
				dma_port_write(core, ch, data);
			}
			address = header & 0x00FFFFFF;
			if (address & (1u << 23)) {
				break;
			}
		}
		return;
	}
	case 3:
		fprintf(stderr, "Unknown sync mode %d!\n", sync_mode);
		return;
	}
}

static inline void dma_maybe_trigger(struct HakuyaCore *core, enum DMAChannelName ch) {
	uint32_t control_reg = core->dma.channel[ch].control;
	switch (ch) {
	case DMA_GPU:
		if (control_reg & (1u << 24)) {
			dma_trigger(core, DMA_GPU);
		}
		break;
	case DMA_OTC:
		if (control_reg & (1u << 28)) {
			dma_trigger_otc(core);
		}
		break;
	}
}

void dma_write(struct HakuyaCore *core, uint32_t relative_address, uint32_t value, int bytes) {
	(void)bytes;
	if (bytes != 4) {
		printf("Tried to write %d bytes\n", bytes);
	}

	unsigned channel = (relative_address >> 4) & 7; // 0-6 are channels, 7 is the general control/interrupt
	unsigned field   = (relative_address >> 2) & 3; // 0 start, 1 blocks, 2 control, 3 unused

	if (channel == 7) {
		if (field == 0)
			core->dma.control = value;
		else if (field == 1)
			core->dma.interrupt = value;
		else
			fprintf(stderr, "Warning: Write to unused DMA register\n");
		return;
	}

	switch (field) {
	case 0: core->dma.channel[channel].start = value; break;
	case 1: core->dma.channel[channel].blocks = value; break;
	case 2:
		core->dma.channel[channel].control = value;
		dma_maybe_trigger(core, channel);
		break;
	default:
		fprintf(stderr, "Warning: Write to unused DMA register\n");
		break;
	}
}
