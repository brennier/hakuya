#ifndef HAKUYA_BUS_H
#define HAKUYA_BUS_H

#include <stdint.h>
#include <stdbool.h>

extern uint8_t ram[2 * 1024 * 1024];
extern uint8_t scratchpad[1024];

uint8_t  bus_read8 (uint32_t address);
uint16_t bus_read16(uint32_t address);
uint32_t bus_read32(uint32_t address);

void bus_write8 (uint32_t address, uint8_t  value);
void bus_write16(uint32_t address, uint16_t value);
void bus_write32(uint32_t address, uint32_t value);

enum InterruptType {
	INTERRUPT_VBLANK = 0,
	INTERRUPT_GPU,
	INTERRUPT_CDROM,
	INTERRUPT_DMA,
	INTERRUPT_TIMER0,
	INTERRUPT_TIMER1,
	INTERRUPT_TIMER2,
	INTERRUPT_JOYPAD,
	INTERRUPT_SIO,
	INTERRUPT_SPU,
	INTERRUPT_LIGHTGUN,
	INTERRUPT_COUNT,
};

void bus_interrupt_set(enum InterruptType interrupt);
bool bus_interrupt_is_pending(void);

#endif // HAKUYA_BUS_H
