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

enum DMARegisterName {
	REG_MDEC_IN_START = 0,
	REG_MDEC_IN_BLOCKS,
	REG_MDEC_IN_CONTROL,
	REG_MDEC_IN_UNUSED,
	REG_MDEC_OUT_START,
	REG_MDEC_OUT_BLOCKS,
	REG_MDEC_OUT_CONTROL,
	REG_MDEC_OUT_UNUSED,
	REG_GPU_START,
	REG_GPU_BLOCKS,
	REG_GPU_CONTROL,
	REG_GPU_UNUSED,
	REG_CDROM_START,
	REG_CDROM_BLOCKS,
	REG_CDROM_CONTROL,
	REG_CDROM_UNUSED,
	REG_SPU_START,
	REG_SPU_BLOCKS,
	REG_SPU_CONTROL,
	REG_SPU_UNUSED,
	REG_PIO_START,
	REG_PIO_BLOCKS,
	REG_PIO_CONTROL,
	REG_PIO_UNUSED,
	REG_OTC_START,
	REG_OTC_BLOCKS,
	REG_OTC_CONTROL,
	REG_OTC_UNUSED,
	REG_GENERAL_CONTROL,
	REG_GENERAL_INTERRUPT,
	REG_UNUSED0,
	REG_UNUSED1,
	REG_COUNT,
};

struct DMA {
	uint32_t registers[REG_COUNT];
};

struct DMA dma = {
	.registers[REG_GENERAL_CONTROL] = (uint32_t)0x07654321u,
};

static inline void dma_trigger_otc(struct HakuyaCore *core) {
	uint32_t address    = dma.registers[REG_OTC_START] & 0x00FFFFFCu;
	uint32_t word_count = dma.registers[REG_OTC_BLOCKS] & 0xFFFFu;
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

static inline void dma_trigger_gpu(struct HakuyaCore *core) {
	uint32_t address = dma.registers[REG_GPU_START] & 0x00FFFFFCu;
	uint32_t sync_mode = (dma.registers[REG_GPU_CONTROL] >> 9) & 0x03;
	printf("DMA GPU triggered: start %08X mode %08X\n", address, sync_mode);
	switch (sync_mode) {
	case 0: {
		uint32_t word_count = dma.registers[REG_GPU_BLOCKS] & 0xFFFFu;
		for (uint32_t i = 0; i < word_count; i++) {
			uint32_t data = bus_read32(core, address + 4*i);
			bus_write32(core, GPU0_ADDRESS, data);
		}
		return;
	}
	case 1: {
		uint32_t block_size  = dma.registers[REG_GPU_BLOCKS] & 0xFFFFu;
		uint32_t block_count = dma.registers[REG_GPU_BLOCKS] >> 16;
		for (uint32_t i = 0; i < block_size * block_count; i++) {
			uint32_t data = bus_read32(core, address + 4*i);
			bus_write32(core, GPU0_ADDRESS, data);
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
				bus_write32(core, GPU0_ADDRESS, data);
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

void dma_write(struct HakuyaCore *core, uint32_t relative_address, uint32_t value, int bytes) {
	(void)bytes;
	if (bytes != 4) {
		printf("Tried to write %d bytes\n", bytes);
	}

	enum DMARegisterName reg = relative_address >> 2;
	dma.registers[reg] = value;

	switch (reg) {
	case REG_GPU_CONTROL:
		if (value & (1u << 24)) {
			dma_trigger_gpu(core);
		}
		break;
	case REG_OTC_CONTROL:
		if (value & (1u << 28)) {
			dma_trigger_otc(core);
		}
		break;
	default:
		break;
	}
}
