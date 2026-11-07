/* xlat.c — среда выполнения переведённого кода (см. xlat.h). */
#include "xlat.h"

/* Принять ждущее прерывание на границе инструкции: адрес возврата — next.
 * Обработчик исполняется (эмулятором или его переводом), пока RTI не вернёт нас в next. */
static void take_interrupt(Nes *n, uint16_t next) {
    uint8_t s0 = n->cpu.s;
    n->cpu.pc = next;
    cpu_interrupt(&n->cpu);
    while (!(n->cpu.pc == next && n->cpu.s == s0)) {
        nes_step(n);
        if (n->cpu.jammed || xlat_stop(n)) { n->xlat_bail = 1; return; }
        if ((uint8_t)(n->cpu.s - s0) - 1u < 0x7Fu) { n->xlat_bail = 1; return; }  /* стек выше: вернулись не туда */
    }
}

void xlat_tick(Nes *n, int count, unsigned wmask) {
    for (int i = 0; i < count; i++) nes_bus_cycle(n, (wmask >> i) & 1);
}

int xlat_check(Nes *n, uint16_t next) {
    n->cpu.pc = next;
    /* решение о прерывании — по состоянию на предпоследнем такте, как в cpu_step */
    if (n->cpu.prev_run_irq || n->cpu.prev_need_nmi) take_interrupt(n, next);
    return n->xlat_bail;
}

int xlat_cycles(Nes *n, int count, unsigned wmask, uint16_t next) {
    n->cpu.pc = next;
    xlat_tick(n, count, wmask);
    return xlat_check(n, next);
}

void xlat_jump(Nes *n, uint16_t target) {
    n->cpu.pc = target;
    nes_hook_fn h = nes_find_hook(n, target);
    if (h) { n->hook_calls++; h(n); }
    else n->xlat_bail = 1;
}

int xlat_branch(Nes *n, int taken, uint16_t from_next, uint16_t to) {
    if (!taken) return xlat_cycles(n, 2, 0, from_next);
    n->cpu.pc = to;
    nes_bus_cycle(n, 0);
    nes_bus_cycle(n, 0);
    /* взятый переход не замечает IRQ, появившийся на последнем такте выборки операнда */
    if (n->cpu.run_irq && !n->cpu.prev_run_irq) n->cpu.run_irq = 0;
    nes_bus_cycle(n, 0);
    if (XCROSS(from_next, to)) nes_bus_cycle(n, 0);
    if (n->cpu.prev_run_irq || n->cpu.prev_need_nmi) take_interrupt(n, to);
    return n->xlat_bail;
}

int xlat_io(Nes *n, uint16_t pc, uint16_t next) {
    uint8_t s0 = n->cpu.s;
    n->cpu.pc = pc;              /* трассировку уже сделала метка инструкции (XT) */
    cpu_step(&n->cpu);
    /* cpu_step уже вошёл в обработчик прерывания — доводим его до возврата */
    while (!(n->cpu.pc == next && n->cpu.s == s0)) {
        nes_step(n);
        if (n->cpu.jammed || xlat_stop(n)) { n->xlat_bail = 1; return 1; }
        if ((uint8_t)(n->cpu.s - s0) - 1u < 0x7Fu) { n->xlat_bail = 1; return 1; }
    }
    return n->xlat_bail;
}

/* Исполнять эмулятором, пока стек не поднимется до уровня s_ret (процедура вернулась). */
static void run_until_return(Nes *n, uint8_t s_ret) {
    for (;;) {
        nes_step(n);
        if (n->cpu.jammed || xlat_stop(n)) return;
        if ((uint8_t)(n->cpu.s - s_ret) < 0x80) return;   /* s >= s_ret (с учётом переноса) */
    }
}

int xlat_call(Nes *n, uint16_t target, uint16_t ret_next) {
    uint8_t s_ret = (uint8_t)(n->cpu.s + 2);
    n->cpu.pc = target;
    nes_hook_fn h = nes_find_hook(n, target);
    if (h) {
        n->hook_calls++;
        h(n);
        if (n->xlat_unwind) return 0;
        if (n->xlat_bail) {
            /* вызванная процедура отдала управление: доводим её эмулятором до возврата */
            n->xlat_bail = 0;
            run_until_return(n, s_ret);
        }
    } else {
        run_until_return(n, s_ret);
    }
    if (n->xlat_unwind) { n->xlat_bail = 1; return 0; }
    if (n->cpu.pc == ret_next && n->cpu.s == s_ret) return 1;
    /* процедура вернулась не туда (трюк со стеком или таблица переходов):
     * дальше — эмулятор, переведённые функции выше по цепочке тоже выходят */
    n->xlat_bail = 1;
    return 0;
}

int xlat_far(Nes *n, uint16_t after) {
    /* переходник снимает адрес возврата со стека, поэтому «подъём стека» здесь не признак
     * возврата: ждём ровно возврата за параметры на исходном уровне стека */
    uint8_t s_ret = (uint8_t)(n->cpu.s + 2);
    while (!(n->cpu.pc == after && n->cpu.s == s_ret)) {
        nes_step(n);
        if (n->cpu.jammed || xlat_stop(n)) { n->xlat_bail = 1; return 0; }
        /* стек поднялся выше уровня вызова: вызванный код ушёл не через переходник */
        if ((uint8_t)(n->cpu.s - s_ret) - 1u < 0x7Fu) { n->xlat_bail = 1; return 0; }
    }
    return 1;
}

void xlat_bail_to(Nes *n, uint16_t pc) {
    n->cpu.pc = pc;
    n->xlat_bail = 1;
}
