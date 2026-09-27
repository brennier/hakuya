#include "bus.h"
#include "bios.h"
#include "debug.h"

#include <assert.h>
#include <stdio.h>
#include <stdbool.h>

static uint8_t ram[2 * 1024 * 1024] = { 0 };

static inline uint32_t ram_read(uint32_t address, int bytes) {
	uint32_t value = 0;
	for (int i = 0; i < bytes; i++)
		value |= (uint32_t)(ram[address + i]) << (8 * i);
	return value;
}

static inline void ram_write(uint32_t address, uint32_t value, int bytes) {
	for (int i = 0; i < bytes; i++)
		ram[address + i] = (uint8_t)(value >> (8 * i));
}

typedef struct {
	uint32_t start;
	uint32_t size;
} MemoryRange;

static const MemoryRange RAM        = { 0x00000000, 2 * 1024 * 1024 };
static const MemoryRange EXPANSION1 = { 0x1F000000, 8 * 1024 * 1024 };
static const MemoryRange MEM_CTRL   = { 0x1F801000, 36 };
static const MemoryRange RAM_SIZE   = { 0x1F801060, 4 };
static const MemoryRange SPU        = { 0x1F801C00, 640 };
static const MemoryRange EXPANSION2 = { 0x1F802000, 8 * 1024 };
static const MemoryRange BIOS       = { 0x1FC00000, 512 * 1024 };
static const MemoryRange CACHE_CTRL = { 0xFFFE0130, 4 };

static inline bool range_contains(MemoryRange range, uint32_t address) {
	return (address >= range.start && address < range.start + range.size);
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

static inline uint32_t bus_read(uint32_t address, int bytes) {
	uint32_t phys_address = bus_strip_region_bits(address);

	if (range_contains(RAM, phys_address))
		return ram_read(phys_address - RAM.start, bytes);
	else if (range_contains(EXPANSION1, phys_address)) {
		fprintf(stderr, "[WARNING] read%d from EXPANSION1 at %08X. Returning all 1's.\n", bytes * 8, address);
		return 0xFFFFFFFF;
	} else if (range_contains(BIOS, phys_address))
		return bios_read(phys_address - BIOS.start, bytes);

	PANIC("Unimplemented read at address %08X\n", address);
}

static inline void bus_write(uint32_t address, uint32_t value, int bytes) {
	uint32_t phys_address = bus_strip_region_bits(address);

	if (range_contains(RAM, phys_address))
		ram_write(phys_address - RAM.start, value, bytes);
	else if (range_contains(MEM_CTRL, phys_address))
		fprintf(stderr, "[WARNING] Ignored write%d to MEM_CTRL at %08X = %08X\n", bytes * 8, address, value);
	else if (range_contains(RAM_SIZE, phys_address))
		fprintf(stderr, "[WARNING] Ignored write%d to RAM_SIZE at %08X = %08X\n", bytes * 8, address, value);
	else if (range_contains(CACHE_CTRL, phys_address))
		fprintf(stderr, "[WARNING] Ignored write%d to CACHE_CTRL at %08X = %08X\n", bytes * 8, address, value);
	else if (range_contains(SPU, phys_address))
		fprintf(stderr, "[WARNING] Ignored write%d to SPU at %08X = %08X\n", bytes * 8, address, value);
	else if (range_contains(EXPANSION2, phys_address))
		fprintf(stderr, "[WARNING] Ignored write%d to EXPANSION2 at %08X = %08X\n", bytes * 8, address, value);
	else
		PANIC("Unimplemented write at address %08X\n", address);
}

uint8_t  bus_read8 (uint32_t address) { return bus_read(address, 1); }
uint16_t bus_read16(uint32_t address) { return bus_read(address, 2); }
uint32_t bus_read32(uint32_t address) { return bus_read(address, 4); }

void bus_write8 (uint32_t address, uint8_t  value) { bus_write(address, value, 1); }
void bus_write16(uint32_t address, uint16_t value) { bus_write(address, value, 2); }
void bus_write32(uint32_t address, uint32_t value) { bus_write(address, value, 4); }
