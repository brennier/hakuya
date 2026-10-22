#ifndef HAKUYA_BIOS_H
#define HAKUYA_BIOS_H

#include <stdint.h>

struct HakuyaCore;

void bios_load(struct HakuyaCore *core, const char *filepath);
uint32_t bios_read(struct HakuyaCore *core, uint32_t offset, int bytes);

#endif // HAKUYA_BIOS_H
