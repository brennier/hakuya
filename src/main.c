#include <stdbool.h>
#include <stdio.h>

#include "core.h"
#include "bios.h"

#define BIOS_FILE "./bios/SCPH1001.BIN"

int main(int argc, char *argv[]) {
	switch (argc) {
	case 1: bios_load(BIOS_FILE); break;
	case 2: bios_load(argv[1]);   break;
	default:
		fprintf(stderr, "Usage: hakuya [BIOS_FILE]\n");
		return 1;
	}

	struct HakuyaCore *core = hakuya_core_create();
	while (true) {
		hakuya_core_tick(core);
	}
	hakuya_core_free(core);
	return 0;
}
