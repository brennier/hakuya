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

void hakuya_render_vram_u16(struct HakuyaCore *core, uint16_t *output_buffer);
void hakuya_render_vram_u32(struct HakuyaCore *core, uint32_t *output_buffer);
void hakuya_get_display_area(struct HakuyaCore *core, int *x, int *y, int *width, int *height);

#endif // HAKUYA_H
