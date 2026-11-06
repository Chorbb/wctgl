/* cpu6502.c — потактовый процессор NES.
 *
 * Последовательности обращений к шине по тактам взяты из описания 6502
 * («6502 cycle-by-cycle», nesdev wiki); опрос прерываний устроен как в Mesen:
 * в конце каждого такта запоминается состояние линий, решение о входе
 * в прерывание принимается по состоянию на предпоследнем такте. */
#include "cpu6502.h"

void cpu_end_cycle(Cpu *c) {
    c->prev_need_nmi = c->need_nmi;
    if (!c->prev_nmi_level && c->nmi_level) c->need_nmi = 1;   /* NMI срабатывает по фронту */
    c->prev_nmi_level = c->nmi_level;
    c->prev_run_irq = c->run_irq;
    c->run_irq = c->irq_line && !(c->p & FLAG_I);
}

static inline uint8_t RD(Cpu *c, uint16_t a) {
    uint8_t v = c->read(c->ctx, a);
    cpu_end_cycle(c);
    return v;
}
static inline void WR(Cpu *c, uint16_t a, uint8_t v) {
    c->write(c->ctx, a, v);
    cpu_end_cycle(c);
}

/* Порядок увеличения PC важен только для сверки PC посреди инструкции с Mesen:
 * у кода операции PC растёт после чтения, у операндов — до (как в Mesen). */
static inline uint8_t fetch(Cpu *c) { return RD(c, c->pc++); }
static inline uint8_t fetch_opcode(Cpu *c) { uint8_t v = RD(c, c->pc); c->pc++; return v; }
static inline void dummy_pc(Cpu *c) { RD(c, c->pc); }

void cpu_push(Cpu *c, uint8_t v) { WR(c, (uint16_t)(0x100 | c->s), v); c->s--; }
uint8_t cpu_pull(Cpu *c) { c->s++; return RD(c, (uint16_t)(0x100 | c->s)); }

static inline void setzn(Cpu *c, uint8_t v) {
    c->p = (uint8_t)((c->p & ~(FLAG_Z | FLAG_N)) | (v == 0 ? FLAG_Z : 0) | (v & 0x80));
}

void cpu_init(Cpu *c, cpu_read_fn rd, cpu_write_fn wr, void *ctx) {
    c->a = c->x = c->y = 0;
    c->s = 0xFD;
    c->p = FLAG_I | FLAG_U;
    c->pc = 0;
    c->cycles = 0;
    c->jammed = 0;
    c->nmi_level = c->irq_line = 0;
    c->need_nmi = c->prev_need_nmi = c->prev_nmi_level = 0;
    c->run_irq = c->prev_run_irq = 0;
    c->read = rd; c->write = wr; c->ctx = ctx;
}

void cpu_reset(Cpu *c) {
    /* 7 тактов: 2 пустых чтения, 3 «записи» в стек как чтения, затем вектор */
    dummy_pc(c); dummy_pc(c);
    for (int i = 0; i < 3; i++) { RD(c, (uint16_t)(0x100 | c->s)); c->s--; }
    c->p |= FLAG_I;
    uint8_t lo = RD(c, 0xFFFC);
    uint8_t hi = RD(c, 0xFFFD);
    c->pc = (uint16_t)(lo | (hi << 8));
    c->jammed = 0;
    c->need_nmi = c->prev_need_nmi = 0;
}

/* вход в прерывание (NMI или IRQ), 7 тактов */
static void interrupt(Cpu *c) {
    dummy_pc(c); dummy_pc(c);
    cpu_push(c, (uint8_t)(c->pc >> 8));
    cpu_push(c, (uint8_t)c->pc);
    uint16_t vec;
    if (c->need_nmi) { c->need_nmi = 0; vec = 0xFFFA; }   /* NMI может «перехватить» IRQ */
    else vec = 0xFFFE;
    cpu_push(c, (uint8_t)((c->p | FLAG_U) & ~FLAG_B));
    c->p |= FLAG_I;
    uint8_t lo = RD(c, vec);
    uint8_t hi = RD(c, (uint16_t)(vec + 1));
    c->pc = (uint16_t)(lo | (hi << 8));
}

/* ---------------------------------------------------------------- адресация
 * Каждая функция выполняет такты выборки адреса и возвращает итоговый адрес.
 * Для индексных режимов: rd = 1 — инструкция чтения (пустое чтение только при
 * переходе страницы), rd = 0 — запись или RMW (пустое чтение всегда). */

static inline uint16_t a_zp(Cpu *c) { return fetch(c); }
static inline uint16_t a_zpi(Cpu *c, uint8_t idx) {
    uint8_t z = fetch(c);
    RD(c, z);                                  /* пустое чтение до прибавления индекса */
    return (uint8_t)(z + idx);
}
static inline uint16_t a_abs(Cpu *c) {
    uint8_t lo = fetch(c);
    uint8_t hi = fetch(c);
    return (uint16_t)(lo | (hi << 8));
}
static inline uint16_t a_absi(Cpu *c, uint8_t idx, int rd) {
    uint16_t base = a_abs(c);
    uint16_t a = (uint16_t)(base + idx);
    if (!rd || (base & 0xFF00) != (a & 0xFF00))
        RD(c, (uint16_t)((base & 0xFF00) | (a & 0x00FF)));   /* чтение по «недоправленному» адресу */
    return a;
}
static inline uint16_t a_indx(Cpu *c) {
    uint8_t z = fetch(c);
    RD(c, z);
    z = (uint8_t)(z + c->x);
    uint8_t lo = RD(c, z);
    uint8_t hi = RD(c, (uint8_t)(z + 1));
    return (uint16_t)(lo | (hi << 8));
}
static inline uint16_t a_indy(Cpu *c, int rd) {
    uint8_t z = fetch(c);
    uint8_t lo = RD(c, z);
    uint8_t hi = RD(c, (uint8_t)(z + 1));
    uint16_t base = (uint16_t)(lo | (hi << 8));
    uint16_t a = (uint16_t)(base + c->y);
    if (!rd || (base & 0xFF00) != (a & 0xFF00))
        RD(c, (uint16_t)((base & 0xFF00) | (a & 0x00FF)));
    return a;
}

/* ---------------------------------------------------------------- операции */
static inline void op_adc(Cpu *c, uint8_t m) {
    unsigned s = c->a + m + (c->p & FLAG_C);
    c->p = (uint8_t)(c->p & ~(FLAG_C | FLAG_V));
    if (s > 0xFF) c->p |= FLAG_C;
    if (~(c->a ^ m) & (c->a ^ s) & 0x80) c->p |= FLAG_V;
    c->a = (uint8_t)s; setzn(c, c->a);
}
static inline void op_cmp(Cpu *c, uint8_t r, uint8_t m) {
    c->p = (uint8_t)((c->p & ~FLAG_C) | (r >= m ? FLAG_C : 0));
    setzn(c, (uint8_t)(r - m));
}
static inline uint8_t op_asl(Cpu *c, uint8_t v) { c->p = (uint8_t)((c->p & ~FLAG_C) | (v >> 7)); v <<= 1; setzn(c, v); return v; }
static inline uint8_t op_lsr(Cpu *c, uint8_t v) { c->p = (uint8_t)((c->p & ~FLAG_C) | (v & 1)); v >>= 1; setzn(c, v); return v; }
static inline uint8_t op_rol(Cpu *c, uint8_t v) { uint8_t r = (uint8_t)((v << 1) | (c->p & FLAG_C)); c->p = (uint8_t)((c->p & ~FLAG_C) | (v >> 7)); setzn(c, r); return r; }
static inline uint8_t op_ror(Cpu *c, uint8_t v) { uint8_t r = (uint8_t)((v >> 1) | ((c->p & FLAG_C) << 7)); c->p = (uint8_t)((c->p & ~FLAG_C) | (v & 1)); setzn(c, r); return r; }
static inline uint8_t op_inc(Cpu *c, uint8_t v) { v++; setzn(c, v); return v; }
static inline uint8_t op_dec(Cpu *c, uint8_t v) { v--; setzn(c, v); return v; }

/* чтение-изменение-запись: чтение, запись старого значения, запись нового */
#define RMW(addr_expr, FN) do { \
    uint16_t a_ = (addr_expr); \
    uint8_t v_ = RD(c, a_); \
    WR(c, a_, v_); \
    WR(c, a_, FN(c, v_)); \
} while (0)

static void branch(Cpu *c, int cond) {
    int8_t off = (int8_t)fetch(c);
    if (!cond) return;
    /* взятый переход без смены страницы не опрашивает IRQ на последнем такте
     * (поведение 6502, так же сделано в Mesen) */
    if (c->run_irq && !c->prev_run_irq) c->run_irq = 0;
    dummy_pc(c);
    uint16_t to = (uint16_t)(c->pc + off);
    if ((to & 0xFF00) != (c->pc & 0xFF00))
        RD(c, (uint16_t)((c->pc & 0xFF00) | (to & 0x00FF)));
    c->pc = to;
}

void cpu_step(Cpu *c) {
    if (c->jammed) { dummy_pc(c); return; }

    uint8_t op = fetch_opcode(c);
    uint16_t a;
    uint8_t m;

    switch (op) {
    /* --- загрузки --- */
#define LOADS(OPBASE_IMM, OPZP, OPZPI, OPABS, OPABSI, IDX_ZP, IDX_ABS, REG) \
    case OPBASE_IMM: c->REG = fetch(c); setzn(c, c->REG); break; \
    case OPZP:  c->REG = RD(c, a_zp(c)); setzn(c, c->REG); break; \
    case OPZPI: c->REG = RD(c, a_zpi(c, c->IDX_ZP)); setzn(c, c->REG); break; \
    case OPABS: c->REG = RD(c, a_abs(c)); setzn(c, c->REG); break; \
    case OPABSI: c->REG = RD(c, a_absi(c, c->IDX_ABS, 1)); setzn(c, c->REG); break;
    LOADS(0xA2, 0xA6, 0xB6, 0xAE, 0xBE, y, y, x)
    LOADS(0xA0, 0xA4, 0xB4, 0xAC, 0xBC, x, x, y)
#undef LOADS
    case 0xA9: c->a = fetch(c); setzn(c, c->a); break;
    case 0xA5: c->a = RD(c, a_zp(c)); setzn(c, c->a); break;
    case 0xB5: c->a = RD(c, a_zpi(c, c->x)); setzn(c, c->a); break;
    case 0xAD: c->a = RD(c, a_abs(c)); setzn(c, c->a); break;
    case 0xBD: c->a = RD(c, a_absi(c, c->x, 1)); setzn(c, c->a); break;
    case 0xB9: c->a = RD(c, a_absi(c, c->y, 1)); setzn(c, c->a); break;
    case 0xA1: c->a = RD(c, a_indx(c)); setzn(c, c->a); break;
    case 0xB1: c->a = RD(c, a_indy(c, 1)); setzn(c, c->a); break;

    /* --- записи --- */
    case 0x85: WR(c, a_zp(c), c->a); break;
    case 0x95: WR(c, a_zpi(c, c->x), c->a); break;
    case 0x8D: WR(c, a_abs(c), c->a); break;
    case 0x9D: WR(c, a_absi(c, c->x, 0), c->a); break;
    case 0x99: WR(c, a_absi(c, c->y, 0), c->a); break;
    case 0x81: WR(c, a_indx(c), c->a); break;
    case 0x91: WR(c, a_indy(c, 0), c->a); break;
    case 0x86: WR(c, a_zp(c), c->x); break;
    case 0x96: WR(c, a_zpi(c, c->y), c->x); break;
    case 0x8E: WR(c, a_abs(c), c->x); break;
    case 0x84: WR(c, a_zp(c), c->y); break;
    case 0x94: WR(c, a_zpi(c, c->x), c->y); break;
    case 0x8C: WR(c, a_abs(c), c->y); break;

    /* --- регистры (2 такта: выборка + пустое чтение) --- */
    case 0xAA: dummy_pc(c); c->x = c->a; setzn(c, c->x); break;
    case 0xA8: dummy_pc(c); c->y = c->a; setzn(c, c->y); break;
    case 0x8A: dummy_pc(c); c->a = c->x; setzn(c, c->a); break;
    case 0x98: dummy_pc(c); c->a = c->y; setzn(c, c->a); break;
    case 0xBA: dummy_pc(c); c->x = c->s; setzn(c, c->x); break;
    case 0x9A: dummy_pc(c); c->s = c->x; break;
    case 0xE8: dummy_pc(c); c->x++; setzn(c, c->x); break;
    case 0xC8: dummy_pc(c); c->y++; setzn(c, c->y); break;
    case 0xCA: dummy_pc(c); c->x--; setzn(c, c->x); break;
    case 0x88: dummy_pc(c); c->y--; setzn(c, c->y); break;
    case 0x0A: dummy_pc(c); c->a = op_asl(c, c->a); break;
    case 0x2A: dummy_pc(c); c->a = op_rol(c, c->a); break;
    case 0x4A: dummy_pc(c); c->a = op_lsr(c, c->a); break;
    case 0x6A: dummy_pc(c); c->a = op_ror(c, c->a); break;
    case 0x18: dummy_pc(c); c->p &= (uint8_t)~FLAG_C; break;
    case 0x38: dummy_pc(c); c->p |= FLAG_C; break;
    case 0x58: dummy_pc(c); c->p &= (uint8_t)~FLAG_I; break;
    case 0x78: dummy_pc(c); c->p |= FLAG_I; break;
    case 0xB8: dummy_pc(c); c->p &= (uint8_t)~FLAG_V; break;
    case 0xD8: dummy_pc(c); c->p &= (uint8_t)~FLAG_D; break;
    case 0xF8: dummy_pc(c); c->p |= FLAG_D; break;
    case 0xEA: case 0x1A: case 0x3A: case 0x5A: case 0x7A: case 0xDA: case 0xFA: dummy_pc(c); break;

    /* --- стек --- */
    case 0x48: dummy_pc(c); cpu_push(c, c->a); break;
    case 0x08: dummy_pc(c); cpu_push(c, (uint8_t)(c->p | FLAG_B | FLAG_U)); break;
    case 0x68: dummy_pc(c); RD(c, (uint16_t)(0x100 | c->s)); c->a = cpu_pull(c); setzn(c, c->a); break;
    case 0x28: dummy_pc(c); RD(c, (uint16_t)(0x100 | c->s)); c->p = (uint8_t)((cpu_pull(c) & ~FLAG_B) | FLAG_U); break;

    /* --- арифметика и логика --- */
#define ALU(base, OPERATION) \
    case base + 0x09: m = fetch(c); OPERATION; break; \
    case base + 0x05: m = RD(c, a_zp(c)); OPERATION; break; \
    case base + 0x15: m = RD(c, a_zpi(c, c->x)); OPERATION; break; \
    case base + 0x0D: m = RD(c, a_abs(c)); OPERATION; break; \
    case base + 0x1D: m = RD(c, a_absi(c, c->x, 1)); OPERATION; break; \
    case base + 0x19: m = RD(c, a_absi(c, c->y, 1)); OPERATION; break; \
    case base + 0x01: m = RD(c, a_indx(c)); OPERATION; break; \
    case base + 0x11: m = RD(c, a_indy(c, 1)); OPERATION; break;
    ALU(0x00, (c->a |= m, setzn(c, c->a)))            /* ORA */
    ALU(0x20, (c->a &= m, setzn(c, c->a)))            /* AND */
    ALU(0x40, (c->a ^= m, setzn(c, c->a)))            /* EOR */
    ALU(0x60, op_adc(c, m))                           /* ADC */
    ALU(0xC0, op_cmp(c, c->a, m))                     /* CMP */
    ALU(0xE0, op_adc(c, (uint8_t)~m))                 /* SBC */
#undef ALU
    case 0xEB: m = fetch(c); op_adc(c, (uint8_t)~m); break;

    case 0xE0: op_cmp(c, c->x, fetch(c)); break;
    case 0xE4: op_cmp(c, c->x, RD(c, a_zp(c))); break;
    case 0xEC: op_cmp(c, c->x, RD(c, a_abs(c))); break;
    case 0xC0: op_cmp(c, c->y, fetch(c)); break;
    case 0xC4: op_cmp(c, c->y, RD(c, a_zp(c))); break;
    case 0xCC: op_cmp(c, c->y, RD(c, a_abs(c))); break;
    case 0x24: case 0x2C:
        m = RD(c, op == 0x24 ? a_zp(c) : a_abs(c));
        c->p = (uint8_t)((c->p & ~(FLAG_Z | FLAG_V | FLAG_N)) | (m & 0xC0) | ((c->a & m) ? 0 : FLAG_Z));
        break;

    /* --- чтение-изменение-запись --- */
#define RMW_GROUP(base, FN) \
    case base + 0x06: RMW(a_zp(c), FN); break; \
    case base + 0x16: RMW(a_zpi(c, c->x), FN); break; \
    case base + 0x0E: RMW(a_abs(c), FN); break; \
    case base + 0x1E: RMW(a_absi(c, c->x, 0), FN); break;
    RMW_GROUP(0x00, op_asl)
    RMW_GROUP(0x20, op_rol)
    RMW_GROUP(0x40, op_lsr)
    RMW_GROUP(0x60, op_ror)
    RMW_GROUP(0xC0, op_dec)
    RMW_GROUP(0xE0, op_inc)
#undef RMW_GROUP

    /* --- переходы --- */
    case 0x4C: { uint8_t lo = fetch(c); uint8_t hi = RD(c, c->pc); c->pc = (uint16_t)(lo | (hi << 8)); break; }
    case 0x6C: {  /* JMP (abs) с ошибкой 6502 на границе страницы */
        uint16_t p = a_abs(c);
        uint8_t lo = RD(c, p);
        uint8_t hi = RD(c, (uint16_t)((p & 0xFF00) | (uint8_t)(p + 1)));
        c->pc = (uint16_t)(lo | (hi << 8));
        break;
    }
    case 0x20: {
        uint8_t lo = fetch(c);
        RD(c, (uint16_t)(0x100 | c->s));               /* внутренний такт */
        cpu_push(c, (uint8_t)(c->pc >> 8));
        cpu_push(c, (uint8_t)c->pc);
        uint8_t hi = RD(c, c->pc);
        c->pc = (uint16_t)(lo | (hi << 8));
        break;
    }
    case 0x60: {
        dummy_pc(c);
        RD(c, (uint16_t)(0x100 | c->s));
        uint8_t lo = cpu_pull(c);
        uint8_t hi = cpu_pull(c);
        c->pc = (uint16_t)(lo | (hi << 8));
        dummy_pc(c);
        c->pc++;
        break;
    }
    case 0x40: {
        dummy_pc(c);
        RD(c, (uint16_t)(0x100 | c->s));
        c->p = (uint8_t)((cpu_pull(c) & ~FLAG_B) | FLAG_U);
        uint8_t lo = cpu_pull(c);
        uint8_t hi = cpu_pull(c);
        c->pc = (uint16_t)(lo | (hi << 8));
        break;
    }
    case 0x00: {   /* BRK */
        fetch(c);
        cpu_push(c, (uint8_t)(c->pc >> 8));
        cpu_push(c, (uint8_t)c->pc);
        uint16_t vec = 0xFFFE;
        if (c->need_nmi) { c->need_nmi = 0; vec = 0xFFFA; }
        cpu_push(c, (uint8_t)(c->p | FLAG_B | FLAG_U));
        c->p |= FLAG_I;
        uint8_t lo = RD(c, vec);
        uint8_t hi = RD(c, (uint16_t)(vec + 1));
        c->pc = (uint16_t)(lo | (hi << 8));
        c->prev_need_nmi = 0;
        break;
    }

    case 0x10: branch(c, !(c->p & FLAG_N)); break;
    case 0x30: branch(c,  (c->p & FLAG_N)); break;
    case 0x50: branch(c, !(c->p & FLAG_V)); break;
    case 0x70: branch(c,  (c->p & FLAG_V)); break;
    case 0x90: branch(c, !(c->p & FLAG_C)); break;
    case 0xB0: branch(c,  (c->p & FLAG_C)); break;
    case 0xD0: branch(c, !(c->p & FLAG_Z)); break;
    case 0xF0: branch(c,  (c->p & FLAG_Z)); break;

    /* --- неофициальные NOP с операндом --- */
    case 0x80: case 0x82: case 0x89: case 0xC2: case 0xE2: fetch(c); break;
    case 0x04: case 0x44: case 0x64: RD(c, a_zp(c)); break;
    case 0x14: case 0x34: case 0x54: case 0x74: case 0xD4: case 0xF4: RD(c, a_zpi(c, c->x)); break;
    case 0x0C: RD(c, a_abs(c)); break;
    case 0x1C: case 0x3C: case 0x5C: case 0x7C: case 0xDC: case 0xFC: RD(c, a_absi(c, c->x, 1)); break;

    /* --- неофициальные, которые иногда встречаются --- */
    case 0xA7: c->a = c->x = RD(c, a_zp(c)); setzn(c, c->a); break;   /* LAX */
    case 0xAF: c->a = c->x = RD(c, a_abs(c)); setzn(c, c->a); break;
    case 0x87: WR(c, a_zp(c), c->a & c->x); break;                     /* SAX */
    case 0x8F: WR(c, a_abs(c), c->a & c->x); break;
    case 0xC7: a = a_zp(c);  m = RD(c, a); WR(c, a, m); m--; WR(c, a, m); op_cmp(c, c->a, m); break;  /* DCP */
    case 0xCF: a = a_abs(c); m = RD(c, a); WR(c, a, m); m--; WR(c, a, m); op_cmp(c, c->a, m); break;
    case 0xE7: a = a_zp(c);  m = RD(c, a); WR(c, a, m); m++; WR(c, a, m); op_adc(c, (uint8_t)~m); break; /* ISB */
    case 0xEF: a = a_abs(c); m = RD(c, a); WR(c, a, m); m++; WR(c, a, m); op_adc(c, (uint8_t)~m); break;

    default:
        /* неизвестная инструкция: останавливаемся, чтобы тест сразу это показал */
        c->jammed = 1;
        c->pc--;
        return;
    }

    /* решение о прерывании — по состоянию на предпоследнем такте */
    if (c->prev_run_irq || c->prev_need_nmi) interrupt(c);
}
