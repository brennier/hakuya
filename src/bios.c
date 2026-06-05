#include "bios.h"
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#define BIOS_FILE "./bios/SCPH1001.BIN"
#define BIOS_SIZE (512u * 1024u)

uint8_t bios[BIOS_SIZE] = { 0 };
bool bios_is_setup = false;

static void bios_setup(void) {
	FILE *file_ptr = fopen(BIOS_FILE, "rb");
	if (!file_ptr) {
		fprintf(stderr, "Failed to allocate space for the BIOS\n");
		exit(EXIT_FAILURE);
	}
	size_t length = fread(bios, sizeof(char), BIOS_SIZE, file_ptr);
	if (length != BIOS_SIZE) {
		fprintf(stderr, "Failed read 512KiB from %s\n", BIOS_FILE);
		exit(EXIT_FAILURE);
	}
	fclose(file_ptr);
	bios_is_setup = true;
}

uint8_t bios_read(uint32_t offset) {
	assert(offset < BIOS_SIZE);
	if (!bios_is_setup)
		bios_setup();
	return bios[offset];
}
