#ifndef HAKUYA_BIOS_H
#define HAKUYA_BIOS_H

#include <stdint.h>

void bios_load(const char *filepath);
uint32_t bios_read(uint32_t offset, int bytes);

#endif // HAKUYA_BIOS_H
