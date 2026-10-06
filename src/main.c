#include <stdbool.h>
#include <stdio.h>

#include "core.h"

#define BIOS_FILE "./bios/SCPH1001.BIN"

int main(int argc, char *argv[]) {
	struct HakuyaCore *core = hakuya_core_create();
	switch (argc) {
	case 1: hakuya_load_bios(core, BIOS_FILE); break;
	case 2: hakuya_load_bios(core, argv[1]);   break;
	default:
		fprintf(stderr, "Usage: hakuya [BIOS_FILE]\n");
		return 1;
	}

	while (true) {
		hakuya_core_tick(core);
	}
	hakuya_core_free(core);
	return 0;
}
