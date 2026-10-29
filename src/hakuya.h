#ifndef HAKUYA_H
#define HAKUYA_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

struct HakuyaCore;

struct HakuyaCore *hakuya_core_create(void);
void hakuya_core_free(struct HakuyaCore *core);

void hakuya_core_tick(struct HakuyaCore *core);
bool hakuya_load_bios(struct HakuyaCore *core, const uint8_t *bios_data, size_t bios_length);
void hakuya_start_exe(struct HakuyaCore *core, uint8_t *exe_data, size_t exe_length);

// For debugging purposes. Writes the full size of VRAM (1024x512) in 16-bit color.
void hakuya_render_vram(struct HakuyaCore *core, uint16_t *output_buffer);

// The output_buffer needs to be at least 640x480.
// The actual width and height written are stored in *width and *height.
// The output is XBGR32 (i.e. the least significant byte is red).
void hakuya_render_frame(struct HakuyaCore *core, uint32_t *output_buffer, int *width, int *height);

#endif // HAKUYA_H
