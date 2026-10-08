#ifndef HAKUYA_CORE_H
#define HAKUYA_CORE_H

#include <stdint.h>
#include <stddef.h>

struct HakuyaCore;

struct HakuyaCore *hakuya_core_create(void);
void hakuya_core_free(struct HakuyaCore *core);

void hakuya_core_tick(struct HakuyaCore *core);
void hakuya_load_bios(struct HakuyaCore *core, const char *bios_path);
void hakuya_start_exe(struct HakuyaCore *core, uint8_t *exe_data, size_t exe_length);

void hakuya_render_vram_u16(struct HakuyaCore *core, uint16_t *output_buffer);
void hakuya_render_vram_u32(struct HakuyaCore *core, uint32_t *output_buffer);

#endif // HAGKUYA_CORE_H
