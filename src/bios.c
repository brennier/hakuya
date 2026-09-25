#include "bios.h"
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#define BIOS_SIZE (512u * 1024u) // 512KiB

uint8_t bios[BIOS_SIZE] = { 0 };
bool bios_is_loaded = false;

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

uint8_t bios_read(uint32_t offset) {
	assert(offset < BIOS_SIZE);
	assert(bios_is_loaded);
	return bios[offset];
}
