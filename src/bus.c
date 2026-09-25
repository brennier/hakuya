#include "bus.h"
#include "bios.h"
#include "debug.h"

#include <assert.h>
#include <stdio.h>
#include <stdbool.h>

typedef struct {
	uint32_t start;
	uint32_t size;
} MemoryRange;

static const MemoryRange RAM        = { 0x00000000, 2 * 1024 * 1024 };
static const MemoryRange MEM_CTRL   = { 0x1F801000, 36 };
static const MemoryRange RAM_SIZE   = { 0x1F801060, 4 };
static const MemoryRange BIOS       = { 0x1FC00000, 512 * 1024 };
static const MemoryRange CACHE_CTRL = { 0xFFFE0130, 4 };

static inline bool range_contains(MemoryRange range, uint32_t address) {
	return (address >= range.start && address < range.start + range.size);
}

uint8_t ram[2 * 1024 * 1024] = { 0 };

uint8_t ram_read(uint32_t address) {
	assert(address < 2 * 1024 * 1024);
	return ram[address];
}

void ram_write(uint32_t address, uint8_t value) {
	assert(address < 2 * 1024 * 1024);
	ram[address] = value;
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

uint8_t bus_read8(uint32_t address) {
	uint32_t phys_address = bus_strip_region_bits(address);

	if (range_contains(RAM, phys_address))
		return ram_read(phys_address - RAM.start);
	if (range_contains(BIOS, phys_address))
		return bios_read(phys_address - BIOS.start);

	PANIC("Unimplemented read at address %08X\n", address);
}

void bus_write8(uint32_t address, uint8_t value) {
	uint32_t phys_address = bus_strip_region_bits(address);

	if (range_contains(RAM, phys_address))
		ram_write(phys_address - RAM.start, value);
	else if (range_contains(MEM_CTRL, phys_address))
		fprintf(stderr, "[WARNING] Write to MEM_CONTROL at %08X\n", address);
	else if (range_contains(RAM_SIZE, phys_address))
		fprintf(stderr, "[WARNING] Write to RAM_SIZE at %08X\n", address);
	else if (range_contains(CACHE_CTRL, phys_address))
		fprintf(stderr, "[WARNING] Write to CACHE_CONTROL at %08X\n", address);
	else
		PANIC("Unimplemented write at address %08X\n", address);
}

uint32_t bus_read32(uint32_t address) {
	uint32_t byte0 = bus_read8(address+0);
	uint32_t byte1 = bus_read8(address+1);
	uint32_t byte2 = bus_read8(address+2);
	uint32_t byte3 = bus_read8(address+3);
	return (byte3 << 24) | (byte2 << 16) | (byte1 << 8) | byte0;
}

void bus_write16(uint32_t address, uint16_t value) {
	fprintf(stderr, "[WARNING] Write to %08X\n", address);
	bus_write8(address+0, value & 0xFF);
	value >>= 8;
	bus_write8(address+1, value & 0xFF);
}

void bus_write32(uint32_t address, uint32_t value) {
	fprintf(stderr, "[WARNING] Write to %08X\n", address);
	bus_write8(address+0, value & 0xFF);
	value >>= 8;
	bus_write8(address+1, value & 0xFF);
	value >>= 8;
	bus_write8(address+2, value & 0xFF);
	value >>= 8;
	bus_write8(address+3, value & 0xFF);
}
