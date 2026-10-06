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
const uint32_t *hakuya_get_frame(struct HakuyaCore *core);

#endif // HAGKUYA_CORE_H
