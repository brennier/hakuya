#include <stdbool.h>
#include <stdio.h>
#include <SDL3/SDL.h>

#include "core.h"

#define BIOS_FILE "./bios/SCPH1001.BIN"
#define EXE_FILE  "./exe/psxtest_cpu.exe"

#define WINDOW_TITLE "Hakuya PSX Emulator"
#define WINDOW_WIDTH  1024
#define WINDOW_HEIGHT 512
#define APP_VERSION "0.1"

// Approximately 33 MHz
#define PSX_CLOCK_SPEED (1 << 25)

struct HakuyaApp {
	SDL_Window *window;
	SDL_Renderer *renderer;
	SDL_Texture *screen_texture;
	SDL_Event event;

	struct HakuyaCore *core;
	uint8_t *exe_data;
	size_t exe_length;
};

static bool hakuya_setup(struct HakuyaApp *app) {
	SDL_SetAppMetadata(WINDOW_TITLE, APP_VERSION, NULL);

	if (!SDL_Init(SDL_INIT_VIDEO)) {
		fprintf(stderr, "Error initializing SDL3: %s\n", SDL_GetError());
		return false;
	}

	app->window = SDL_CreateWindow(WINDOW_TITLE, WINDOW_WIDTH, WINDOW_HEIGHT, 0);
	if (!app->window) {
		fprintf(stderr, "Error creating Window: %s\n", SDL_GetError());
		return false;
	}

	app->renderer = SDL_CreateRenderer(app->window, NULL);
	if (!app->renderer) {
		fprintf(stderr, "Error creating Renderer: %s\n", SDL_GetError());
		return false;
	}

	if (!SDL_SetRenderVSync(app->renderer, 1)) {
		fprintf(stderr, "Error failed to set vsync: %s\n", SDL_GetError());
		return false;
	}

	app->screen_texture = SDL_CreateTexture(app->renderer,
						SDL_PIXELFORMAT_XBGR1555,
						SDL_TEXTUREACCESS_STREAMING,
						WINDOW_WIDTH, WINDOW_HEIGHT);
	if (!app->screen_texture) {
		fprintf(stderr, "Error creating screen texture: %s\n", SDL_GetError());
		return false;
	}

	app->core = hakuya_core_create();
	return true;
}

static void hakuya_cleanup(struct HakuyaApp *app) {
	printf("Cleaning up!\n");
	hakuya_core_free(app->core);
	SDL_free(app->exe_data);
	SDL_DestroyTexture(app->screen_texture);
	SDL_DestroyRenderer(app->renderer);
	SDL_DestroyWindow(app->window);
	SDL_Quit();
}

int main(int argc, char *argv[]) {
	char *bios_path = NULL;
	char *exe_path  = NULL;
	for (int i = 1; i + 1 < argc; i += 2) {
		if (strcmp(argv[i], "-b") == 0) {
			bios_path = argv[i+1];
		} else if (strcmp(argv[i], "-e") == 0) {
			exe_path = argv[i+1];
		} else {
			fprintf(stderr, "Error: Unknown flag '%s'\n", argv[i]);
			fprintf(stderr, "Usage: hakuya -b BIOS_FILE -e [EXE_FILE]\n");
			return 1;
		}
	}
	if (argc % 2 != 1) {
		fprintf(stderr, "Error: Unnecessary extra argument '%s'\n", argv[argc-1]);
		return 1;
	} else if (!bios_path) {
		fprintf(stderr, "Error: Please specify a BIOS file\n");
		fprintf(stderr, "Usage: hakuya -b BIOS_FILE -e [EXE_FILE]\n");
		return 1;
	}

	struct HakuyaApp app = { 0 };
	if (!hakuya_setup(&app)) {
		fprintf(stderr, "Error: Failed to setup the Hakuya App\n");
		return 1;
	}

	hakuya_load_bios(app.core, bios_path);

	if (exe_path) {
		app.exe_data = SDL_LoadFile(exe_path, &app.exe_length);
		if (!app.exe_data) {
			fprintf(stderr, "Error: Failed to load the file '%s'\n", exe_path);
			hakuya_cleanup(&app);
			return 1;
		}
		hakuya_start_exe(app.core, app.exe_data, app.exe_length);
	}

	uint16_t frame[512][1024];
	bool running = true;
	while (running) {
		while (SDL_PollEvent(&app.event)) {
			if (app.event.type == SDL_EVENT_QUIT) {
				running = false;
			}
		}

		for (int clocks = 0; clocks < PSX_CLOCK_SPEED / 60; clocks += 2) {
			hakuya_core_tick(app.core);
		}

		hakuya_render_vram_u16(app.core, (uint16_t*)frame);
		SDL_UpdateTexture(app.screen_texture, NULL, frame, sizeof(uint16_t) * 1024);
		SDL_RenderTexture(app.renderer, app.screen_texture, NULL, NULL);
		SDL_RenderPresent(app.renderer);
	}

	hakuya_cleanup(&app);
	return 0;
}
