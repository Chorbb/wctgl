/* cpu6502.h — процессор NES (Ricoh 2A03, ядро MOS 6502 без десятичного режима).
 *
 * Эмулятор нужен порту как «подложка»: пока процедура игры не переведена на C,
 * её исполняет этот процессор. Переведённые процедуры подменяют оригинальные
 * по адресу (см. hooks в nes.h).
 */
#ifndef CPU6502_H
#define CPU6502_H

#include <stdint.h>

typedef uint8_t  (*cpu_read_fn)(void *ctx, uint16_t addr);
typedef void     (*cpu_write_fn)(void *ctx, uint16_t addr, uint8_t value);

enum {
    FLAG_C = 0x01, FLAG_Z = 0x02, FLAG_I = 0x04, FLAG_D = 0x08,
    FLAG_B = 0x10, FLAG_U = 0x20, FLAG_V = 0x40, FLAG_N = 0x80
};

typedef struct Cpu {
    uint8_t  a, x, y, s, p;
    uint16_t pc;
    uint64_t cycles;          /* всего выполнено тактов */
    int      nmi_pending;     /* фронт NMI ждёт обработки */
    int      irq_line;        /* уровень IRQ (0/1) */
    int      jammed;          /* процессор встал на недопустимой инструкции */
    cpu_read_fn  read;
    cpu_write_fn write;
    void    *ctx;
} Cpu;

void cpu_init(Cpu *c, cpu_read_fn rd, cpu_write_fn wr, void *ctx);
void cpu_reset(Cpu *c);
/* Выполняет одну инструкцию (или вход в прерывание). Возвращает число тактов. */
int  cpu_step(Cpu *c);
void cpu_nmi(Cpu *c);

/* Вспомогательные функции для переведённого кода: стек и флаги как у процессора. */
void    cpu_push(Cpu *c, uint8_t v);
uint8_t cpu_pull(Cpu *c);

#endif
