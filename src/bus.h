#ifndef HAKUYA_BUS_H
#define HAKUYA_BUS_H

#include <stdint.h>

uint8_t  bus_read8 (uint32_t address);
uint16_t bus_read16(uint32_t address);
uint32_t bus_read32(uint32_t address);

void bus_write8 (uint32_t address, uint8_t  value);
void bus_write16(uint32_t address, uint16_t value);
void bus_write32(uint32_t address, uint32_t value);

#endif // HAKUYA_BUS_H
