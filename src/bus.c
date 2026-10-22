#include "core.h"
#include "debug.h"

#include <assert.h>
#include <stdio.h>

typedef void (*WriteHandler)(struct HakuyaCore *core, uint32_t address, uint32_t value, int bytes);
typedef uint32_t (*ReadHandler)(struct HakuyaCore *core, uint32_t address, int bytes);

static inline uint32_t ram_read(struct HakuyaCore *core, uint32_t address, int bytes) {
	uint32_t value = 0;
	for (size_t i = 0; i < (size_t)bytes; i++)
		value |= (uint32_t)(core->ram[address + i]) << (8 * i);
	return value;
}

static inline void ram_write(struct HakuyaCore *core, uint32_t address, uint32_t value, int bytes) {
	for (size_t i = 0; i < (size_t)bytes; i++)
		core->ram[address + i] = (uint8_t)(value >> (8 * i));
}

static inline uint32_t scratchpad_read(struct HakuyaCore *core, uint32_t address, int bytes) {
	uint32_t value = 0;
	for (size_t i = 0; i < (size_t)bytes; i++)
		value |= (uint32_t)(core->scratchpad[address + i]) << (8 * i);
	return value;
}

static inline void scratchpad_write(struct HakuyaCore *core, uint32_t address, uint32_t value, int bytes) {
	for (size_t i = 0; i < (size_t)bytes; i++)
		core->scratchpad[address + i] = (uint8_t)(value >> (8 * i));
}

static inline uint32_t expansion1_read(struct HakuyaCore *core, uint32_t address, int bytes) {
	fprintf(stderr, "[WARNING] Ignoring read%-3d from %-12s at %08X (returning all 1's)\n",
		bytes * 8, "EXPANSION1", address);
	return 0xFFFFFFFF;
}

static inline void interrupt_write(struct HakuyaCore *core, uint32_t relative_address, uint32_t value, int bytes) {
	(void)bytes;
	value &= 0x7FF;
	if (relative_address < 4) {
		// Writing 0 clears an interrupt, writing 1 has no effect
		core->interrupt_status &= (uint16_t)value;
	} else {
		core->interrupt_mask = (uint16_t)value;
	}
}

static inline uint32_t interrupt_read(struct HakuyaCore *core, uint32_t relative_address, int bytes) {
	(void)bytes;
	if (relative_address < 4) {
		return (uint32_t)core->interrupt_status;
	} else {
		return (uint32_t)core->interrupt_mask;
	}
}

typedef struct {
	const char *name;
	uint32_t start;
	uint32_t size;
	WriteHandler write_handler;
	ReadHandler read_handler;
} MemoryRegion;

static const MemoryRegion MEMORY_REGIONS[] = {
	{ "RAM",        0x00000000, 2 * 1024 * 1024, ram_write, ram_read },
	{ "EXPANSION1", 0x1F000000, 8 * 1024 * 1024, NULL, expansion1_read },
	{ "SCRATCHPAD", 0x1F800000, 1024, scratchpad_write, scratchpad_read },
	{ "MEM_CTRL",   0x1F801000, 36,  NULL, NULL },
	{ "JOYPAD",     0x1F801040, 16,  NULL, NULL },
	{ "RAM_SIZE",   0x1F801060, 4,   NULL, NULL },
	{ "INTERRUPTS", 0x1F801070, 8,   interrupt_write, interrupt_read },
	{ "CDROM",      0x1F801800, 4,   NULL, NULL },
	{ "DMA",        0x1F801080, 128, dma_write, NULL },
	{ "GPU0",       0x1F801810, 4,   gpu_write_gp0, gpu_read_gp0 },
	{ "GPU1",       0x1F801814, 4,   gpu_write_gp1, gpu_read_gp1 },
	{ "TIMER",      0x1F801100, 48,  NULL, NULL },
	{ "SPU",        0x1F801C00, 640, NULL, NULL },
	{ "EXPANSION2", 0x1F802000, 8 * 1024,   NULL, NULL },
	{ "BIOS",       0x1FC00000, 512 * 1024, NULL, bios_read },
	{ "CACHE_CTRL", 0xFFFE0130, 4, NULL, NULL },
};

static inline bool region_contains(MemoryRegion region, uint32_t address) {
	return (address >= region.start && address < region.start + region.size);
}

static inline uint32_t bus_strip_region_bits(uint32_t address) {
	switch (address >> 29) {
	case 0: case 1: case 2: case 3:
		return address & 0xFFFFFFFF; // KUSEG: 2 GB
	case 4:
		return address & 0x7FFFFFFF; // KSEG0: 512 MB
	case 5:
		return address & 0x1FFFFFFF; // KSEG1: 512 MB
	case 6: case 7:
		return address & 0xFFFFFFFF; // KSEG2: 1 GB
	}

	PANIC("%s", "Unreachable path");
}

static inline uint32_t bus_read(struct HakuyaCore *core, uint32_t address, int bytes) {
	uint32_t phys_address = bus_strip_region_bits(address);

	for (size_t i = 0; i < sizeof(MEMORY_REGIONS) / sizeof(MemoryRegion); i++) {
		MemoryRegion region = MEMORY_REGIONS[i];
		if (region_contains(region, phys_address)) {
			if (region.read_handler) {
				return region.read_handler(core, phys_address - region.start, bytes);
			} else {
				fprintf(stderr, "[WARNING] Ignoring read%-3d from %-12s at %08X (returning 0)\n",
					bytes * 8, region.name, address);
				return 0;
			}
		}
	}

	PANIC("Unimplemented read at address %08X\n", address);
}

static inline void bus_write(struct HakuyaCore *core, uint32_t address, uint32_t value, int bytes) {
	uint32_t phys_address = bus_strip_region_bits(address);

	for (size_t i = 0; i < sizeof(MEMORY_REGIONS) / sizeof(MemoryRegion); i++) {
		MemoryRegion region = MEMORY_REGIONS[i];
		if (region_contains(region, phys_address)) {
			if (region.write_handler) {
				region.write_handler(core, phys_address - region.start, value, bytes);
				return;
			} else {
				fprintf(stderr, "[WARNING] Ignoring write%-2d to   %-12s at %08X (= %08X)\n",
					bytes * 8, region.name, address, value);
				return;
			}
		}
	}

	PANIC("Unimplemented write at address %08X\n", address);
}

uint8_t  bus_read8 (struct HakuyaCore *core, uint32_t address) { return (uint8_t) bus_read(core, address, 1); }
uint16_t bus_read16(struct HakuyaCore *core, uint32_t address) { return (uint16_t)bus_read(core, address, 2); }
uint32_t bus_read32(struct HakuyaCore *core, uint32_t address) { return (uint32_t)bus_read(core, address, 4); }

void bus_write8 (struct HakuyaCore *core, uint32_t address, uint8_t  value) { bus_write(core, address, value, 1); }
void bus_write16(struct HakuyaCore *core, uint32_t address, uint16_t value) { bus_write(core, address, value, 2); }
void bus_write32(struct HakuyaCore *core, uint32_t address, uint32_t value) { bus_write(core, address, value, 4); }

void bus_interrupt_set(struct HakuyaCore *core, enum InterruptType interrupt) {
	core->interrupt_status |= (1u << (int)interrupt);
}

bool bus_interrupt_is_pending(struct HakuyaCore *core) {
	return (bool)(core->interrupt_status & core->interrupt_mask);
}
