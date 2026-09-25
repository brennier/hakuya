#include "bus.h"
#include "bios.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#define MEM_CONTROL_START 0x1F801000
#define MEM_CONTROL_SIZE  36
#define BIOS_START 0x1FC00000
#define RAM_START  0x00000000
#define BIOS_SIZE  (512u * 1024u)
#define RAM_SIZE_CONTROL 0x1F801060
#define CACHE_CONTROL 0xFFFE0130

uint8_t ram[2 * 1024 * 1024] = { 0 };

uint8_t ram_read(uint32_t address) {
	assert(address < 2 * 1024 * 1024);
	return ram[address];
}

void ram_write(uint32_t address, uint8_t value) {
	assert(address < 2 * 1024 * 1024);
	ram[address] = value;
}

static inline void bus_strip_region_bits(uint32_t *address) {
	switch (*address >> 29) {
	case 0: case 1: case 2: case 3:
		// KUSEG: 2 GB
		*address &= 0xFFFFFFFF; break;
	case 4:
		// KSEG0: 512 MB
		*address &= 0x7FFFFFFF; break;
	case 5:
		// KSEG1: 512 MB
		*address &= 0x1FFFFFFF; break;
	case 6: case 7:
		// KSEG2: 1 GB
		*address &= 0xFFFFFFFF; break;
	}
}

uint8_t bus_read8(uint32_t address) {
	bus_strip_region_bits(&address);
	switch (address & 0xFFC00000) {
	case BIOS_START: return bios_read(address - BIOS_START);
	case RAM_START:  return ram_read(address - RAM_START);
	default:
		fprintf(stderr, "Unimplemented address %08X\n", address);
		exit(EXIT_FAILURE);
	}
}

void bus_write8(uint32_t address, uint8_t value) {
	bus_strip_region_bits(&address);

	if (address >= MEM_CONTROL_START && address < MEM_CONTROL_START + MEM_CONTROL_SIZE) {
		fprintf(stderr, "[WARNING] Write to MEM_CONTROL at %08X\n", address);
		return;
	}

	if (address >= RAM_SIZE_CONTROL && address < RAM_SIZE_CONTROL + 4) {
		fprintf(stderr, "[WARNING] Write to RAM_SIZE_CONTROL at %08X\n", address);
		return;
	}

	if (address >= CACHE_CONTROL && address < CACHE_CONTROL + 4) {
		fprintf(stderr, "[WARNING] Write to CACHE_CONTROL at %08X\n", address);
		return;
	}

	switch (address & 0xFFC00000) {
	case RAM_START: ram_write(address - RAM_START, value); break;
	default:
		fprintf(stderr, "Unimplemented address %08X\n", address);
		exit(EXIT_FAILURE);
	}
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
