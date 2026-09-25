#ifndef HAKUYA_BIOS_H
#define HAKUYA_BIOS_H

#include <stdint.h>

void bios_load(const char *filepath);
uint8_t bios_read(uint32_t offset);

#endif // HAGKUYA_BIOS_H
