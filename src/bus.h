#ifndef HAKUYA_BUS_H
#define HAKUYA_BUS_H

#include <stdint.h>
#include <stdbool.h>

struct HakuyaCore;

uint8_t  bus_read8 (struct HakuyaCore *core, uint32_t address);
uint16_t bus_read16(struct HakuyaCore *core, uint32_t address);
uint32_t bus_read32(struct HakuyaCore *core, uint32_t address);

void bus_write8 (struct HakuyaCore *core, uint32_t address, uint8_t  value);
void bus_write16(struct HakuyaCore *core, uint32_t address, uint16_t value);
void bus_write32(struct HakuyaCore *core, uint32_t address, uint32_t value);

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

void bus_interrupt_set(struct HakuyaCore *core, enum InterruptType interrupt);
bool bus_interrupt_is_pending(struct HakuyaCore *core);

#endif // HAKUYA_BUS_H
