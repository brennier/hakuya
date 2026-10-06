#ifndef HAKUYA_CORE_H
#define HAKUYA_CORE_H

struct HakuyaCore;

struct HakuyaCore *hakuya_core_create(void);
void hakuya_core_free(struct HakuyaCore *core);

void hakuya_core_tick(struct HakuyaCore *core);
void hakuya_load_bios(struct HakuyaCore *core, const char *bios_path);

#endif // HAGKUYA_CORE_H
