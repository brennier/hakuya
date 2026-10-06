#include <stdbool.h>
#include <stdio.h>
#include <SDL3/SDL.h>

#include "core.h"

#define BIOS_FILE "./bios/SCPH1001.BIN"
#define EXE_FILE  "psxtest_cpu.exe"

int main(int argc, char *argv[]) {
	struct HakuyaCore *core = hakuya_core_create();
	switch (argc) {
	case 1: hakuya_load_bios(core, BIOS_FILE); break;
	case 2: hakuya_load_bios(core, argv[1]);   break;
	default:
		fprintf(stderr, "Usage: hakuya [BIOS_FILE]\n");
		return 1;
	}

	size_t exe_length;
	uint8_t *exe_data = SDL_LoadFile(EXE_FILE, &exe_length);
	if (!exe_data) {
		SDL_Log("Failed to load the file %s", EXE_FILE);
		return 1;
	}
	hakuya_run_exe(core, exe_data, exe_length);
	hakuya_core_free(core);
	return 0;
}
