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
						SDL_PIXELFORMAT_XBGR8888,
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
	hakuya_core_free(app->core);
	SDL_free(app->exe_data);

	app->core = NULL;
	app->exe_data = NULL;
	app->exe_length = 0;
}

int main(int argc, char *argv[]) {
	struct HakuyaApp app = { 0 };
	if (!hakuya_setup(&app)) {
		fprintf(stderr, "Failed to setup the Hakuya App");
		return 1;
	}

	switch (argc) {
	case 1: hakuya_load_bios(app.core, BIOS_FILE); break;
	case 2: hakuya_load_bios(app.core, argv[1]);   break;
	default:
		fprintf(stderr, "Usage: hakuya [BIOS_FILE]\n");
		return 1;
	}

	app.exe_data = SDL_LoadFile(EXE_FILE, &app.exe_length);
	if (!app.exe_data) {
		SDL_Log("Failed to load the file %s", EXE_FILE);
		hakuya_cleanup(&app);
		return 1;
	}
	hakuya_start_exe(app.core, app.exe_data, app.exe_length);

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

		const uint32_t *frame = hakuya_get_frame(app.core);
		SDL_UpdateTexture(app.screen_texture, NULL, frame, sizeof(uint32_t) * 1024);
		SDL_RenderTexture(app.renderer, app.screen_texture, NULL, NULL);
		SDL_RenderPresent(app.renderer);
	}

	hakuya_cleanup(&app);
	return 0;
}
