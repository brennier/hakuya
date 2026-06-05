#ifndef HAKUYA_CPU_H
#define HAKUYA_CPU_H

struct HakuyaCPU;

struct HakuyaCPU *cpu_init(void);
void cpu_free(struct HakuyaCPU *cpu);

void cpu_reset(struct HakuyaCPU *cpu);
void run_next_instruction(struct HakuyaCPU *cpu);

#endif // HAGKUYA_BIOS_H
