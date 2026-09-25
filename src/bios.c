#include "bios.h"
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#define BIOS_SIZE (512u * 1024u) // 512KiB

static uint8_t bios[BIOS_SIZE] = { 0 };
static bool bios_is_loaded = false;

void bios_load(const char *filepath) {
	FILE *file_ptr = fopen(filepath, "rb");
	if (!file_ptr) {
		fprintf(stderr, "Failed to open the BIOS file '%s': ", filepath);
		perror("");
		exit(EXIT_FAILURE);
	}
	size_t length = fread(bios, sizeof(char), BIOS_SIZE, file_ptr);
	if (ferror(file_ptr)) {
		fprintf(stderr, "Failed to read the BIOS file '%s': ", filepath);
		perror("");
		fclose(file_ptr);
		exit(EXIT_FAILURE);
	}
	if (length != BIOS_SIZE) {
		fprintf(stderr, "Failed read 512KiB from the BIOS file '%s'\n", filepath);
		fclose(file_ptr);
		exit(EXIT_FAILURE);
	}
	fclose(file_ptr);
	bios_is_loaded = true;
}

uint32_t bios_read(uint32_t offset, int bytes) {
	assert(bios_is_loaded);
	uint32_t value = 0;
	for (int i = 0; i < bytes; i++)
		value |= (uint32_t)(bios[offset + i]) << (8 * i);
	return value;
}
