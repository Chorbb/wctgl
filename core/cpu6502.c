/* cpu6502.c — процессор NES. Точность: по инструкциям и тактам (без точности
 * по отдельным обращениям к шине), этого хватает для покадровой сверки. */
#include "cpu6502.h"

static const uint8_t CYC[256] = {
/*0x*/ 7,6,2,8,3,3,5,5,3,2,2,2,4,4,6,6,
/*1x*/ 2,5,2,8,4,4,6,6,2,4,2,7,4,4,7,7,
/*2x*/ 6,6,2,8,3,3,5,5,4,2,2,2,4,4,6,6,
/*3x*/ 2,5,2,8,4,4,6,6,2,4,2,7,4,4,7,7,
/*4x*/ 6,6,2,8,3,3,5,5,3,2,2,2,3,4,6,6,
/*5x*/ 2,5,2,8,4,4,6,6,2,4,2,7,4,4,7,7,
/*6x*/ 6,6,2,8,3,3,5,5,4,2,2,2,5,4,6,6,
/*7x*/ 2,5,2,8,4,4,6,6,2,4,2,7,4,4,7,7,
/*8x*/ 2,6,2,6,3,3,3,3,2,2,2,2,4,4,4,4,
/*9x*/ 2,6,2,6,4,4,4,4,2,5,2,5,5,5,5,5,
/*Ax*/ 2,6,2,6,3,3,3,3,2,2,2,2,4,4,4,4,
/*Bx*/ 2,5,2,5,4,4,4,4,2,4,2,4,4,4,4,4,
/*Cx*/ 2,6,2,8,3,3,5,5,2,2,2,2,4,4,6,6,
/*Dx*/ 2,5,2,8,4,4,6,6,2,4,2,7,4,4,7,7,
/*Ex*/ 2,6,2,8,3,3,5,5,2,2,2,2,4,4,6,6,
/*Fx*/ 2,5,2,8,4,4,6,6,2,4,2,7,4,4,7,7,
};

#define RD(a)     (c->read(c->ctx, (uint16_t)(a)))
#define WR(a, v)  (c->write(c->ctx, (uint16_t)(a), (uint8_t)(v)))

static inline uint8_t fetch(Cpu *c) { return RD(c->pc++); }
static inline uint16_t fetch16(Cpu *c) { uint8_t lo = fetch(c); return (uint16_t)(lo | (fetch(c) << 8)); }

void cpu_push(Cpu *c, uint8_t v) { WR(0x100 | c->s, v); c->s--; }
uint8_t cpu_pull(Cpu *c) { c->s++; return RD(0x100 | c->s); }

static inline void setzn(Cpu *c, uint8_t v) {
    c->p = (uint8_t)((c->p & ~(FLAG_Z | FLAG_N)) | (v == 0 ? FLAG_Z : 0) | (v & 0x80));
}

void cpu_init(Cpu *c, cpu_read_fn rd, cpu_write_fn wr, void *ctx) {
    c->a = c->x = c->y = 0;
    c->s = 0xFD;
    c->p = FLAG_I | FLAG_U;
    c->pc = 0;
    c->cycles = 0;
    c->nmi_pending = 0;
    c->irq_line = 0;
    c->jammed = 0;
    c->read = rd; c->write = wr; c->ctx = ctx;
}

void cpu_reset(Cpu *c) {
    c->s = (uint8_t)(c->s - 3);
    c->p |= FLAG_I;
    c->pc = (uint16_t)(RD(0xFFFC) | (RD(0xFFFD) << 8));
    c->cycles += 7;
    c->jammed = 0;
}

void cpu_nmi(Cpu *c) { c->nmi_pending = 1; }

static int interrupt(Cpu *c, uint16_t vector, int brk) {
    c->access_offset = 0;
    cpu_push(c, (uint8_t)(c->pc >> 8));
    cpu_push(c, (uint8_t)c->pc);
    cpu_push(c, (uint8_t)((c->p | FLAG_U) | (brk ? FLAG_B : 0)) & (uint8_t)(brk ? 0xFF : ~FLAG_B));
    c->p |= FLAG_I;
    c->pc = (uint16_t)(RD(vector) | (RD(vector + 1) << 8));
    return 7;
}

/* ---- адресация ---- */
static inline uint16_t a_zp(Cpu *c)  { return fetch(c); }
static inline uint16_t a_zpx(Cpu *c) { return (uint8_t)(fetch(c) + c->x); }
static inline uint16_t a_zpy(Cpu *c) { return (uint8_t)(fetch(c) + c->y); }
static inline uint16_t a_abs(Cpu *c) { return fetch16(c); }
static inline uint16_t a_absi(Cpu *c, uint8_t idx, int *cross) {
    uint16_t base = fetch16(c), a = (uint16_t)(base + idx);
    *cross = (base & 0xFF00) != (a & 0xFF00);
    return a;
}
static inline uint16_t a_indx(Cpu *c) {
    uint8_t zp = (uint8_t)(fetch(c) + c->x);
    return (uint16_t)(RD(zp) | (RD((uint8_t)(zp + 1)) << 8));
}
static inline uint16_t a_indy(Cpu *c, int *cross) {
    uint8_t zp = fetch(c);
    uint16_t base = (uint16_t)(RD(zp) | (RD((uint8_t)(zp + 1)) << 8));
    uint16_t a = (uint16_t)(base + c->y);
    *cross = (base & 0xFF00) != (a & 0xFF00);
    return a;
}

/* ---- операции ---- */
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

static int branch(Cpu *c, int cond) {
    int8_t off = (int8_t)fetch(c);
    if (!cond) return 0;
    uint16_t to = (uint16_t)(c->pc + off);
    int extra = 1 + ((to & 0xFF00) != (c->pc & 0xFF00));
    c->pc = to;
    return extra;
}

int cpu_step(Cpu *c) {
    if (c->jammed) { c->cycles += 2; return 2; }
    if (c->nmi_pending) {
        c->nmi_pending = 0;
        c->cycles += 7;
        return interrupt(c, 0xFFFA, 0);
    }
    if (c->irq_line && !(c->p & FLAG_I)) {
        c->cycles += 7;
        return interrupt(c, 0xFFFE, 0);
    }

    uint8_t op = fetch(c);
    int cyc = CYC[op], cross = 0;
    c->access_offset = cyc - 1;   /* чтение/запись регистров обычно на последнем такте */
    uint16_t a;
    uint8_t m;

/* чтение операнда разными способами адресации */
#define READ_OP(mode_block) do { mode_block } while (0)

    switch (op) {
    /* --- LDA --- */
    case 0xA9: c->a = fetch(c); setzn(c, c->a); break;
    case 0xA5: c->a = RD(a_zp(c)); setzn(c, c->a); break;
    case 0xB5: c->a = RD(a_zpx(c)); setzn(c, c->a); break;
    case 0xAD: c->a = RD(a_abs(c)); setzn(c, c->a); break;
    case 0xBD: a = a_absi(c, c->x, &cross); c->a = RD(a); setzn(c, c->a); cyc += cross; break;
    case 0xB9: a = a_absi(c, c->y, &cross); c->a = RD(a); setzn(c, c->a); cyc += cross; break;
    case 0xA1: c->a = RD(a_indx(c)); setzn(c, c->a); break;
    case 0xB1: a = a_indy(c, &cross); c->a = RD(a); setzn(c, c->a); cyc += cross; break;
    /* --- LDX --- */
    case 0xA2: c->x = fetch(c); setzn(c, c->x); break;
    case 0xA6: c->x = RD(a_zp(c)); setzn(c, c->x); break;
    case 0xB6: c->x = RD(a_zpy(c)); setzn(c, c->x); break;
    case 0xAE: c->x = RD(a_abs(c)); setzn(c, c->x); break;
    case 0xBE: a = a_absi(c, c->y, &cross); c->x = RD(a); setzn(c, c->x); cyc += cross; break;
    /* --- LDY --- */
    case 0xA0: c->y = fetch(c); setzn(c, c->y); break;
    case 0xA4: c->y = RD(a_zp(c)); setzn(c, c->y); break;
    case 0xB4: c->y = RD(a_zpx(c)); setzn(c, c->y); break;
    case 0xAC: c->y = RD(a_abs(c)); setzn(c, c->y); break;
    case 0xBC: a = a_absi(c, c->x, &cross); c->y = RD(a); setzn(c, c->y); cyc += cross; break;
    /* --- STA / STX / STY --- */
    case 0x85: WR(a_zp(c), c->a); break;
    case 0x95: WR(a_zpx(c), c->a); break;
    case 0x8D: WR(a_abs(c), c->a); break;
    case 0x9D: WR(a_absi(c, c->x, &cross), c->a); break;
    case 0x99: WR(a_absi(c, c->y, &cross), c->a); break;
    case 0x81: WR(a_indx(c), c->a); break;
    case 0x91: WR(a_indy(c, &cross), c->a); break;
    case 0x86: WR(a_zp(c), c->x); break;
    case 0x96: WR(a_zpy(c), c->x); break;
    case 0x8E: WR(a_abs(c), c->x); break;
    case 0x84: WR(a_zp(c), c->y); break;
    case 0x94: WR(a_zpx(c), c->y); break;
    case 0x8C: WR(a_abs(c), c->y); break;
    /* --- переносы между регистрами --- */
    case 0xAA: c->x = c->a; setzn(c, c->x); break;
    case 0xA8: c->y = c->a; setzn(c, c->y); break;
    case 0x8A: c->a = c->x; setzn(c, c->a); break;
    case 0x98: c->a = c->y; setzn(c, c->a); break;
    case 0xBA: c->x = c->s; setzn(c, c->x); break;
    case 0x9A: c->s = c->x; break;
    /* --- стек --- */
    case 0x48: cpu_push(c, c->a); break;
    case 0x08: cpu_push(c, (uint8_t)(c->p | FLAG_B | FLAG_U)); break;
    case 0x68: c->a = cpu_pull(c); setzn(c, c->a); break;
    case 0x28: c->p = (uint8_t)((cpu_pull(c) & ~FLAG_B) | FLAG_U); break;

#define ALU(base, OPERATION) \
    case base + 0x09: m = fetch(c); OPERATION; break; \
    case base + 0x05: m = RD(a_zp(c)); OPERATION; break; \
    case base + 0x15: m = RD(a_zpx(c)); OPERATION; break; \
    case base + 0x0D: m = RD(a_abs(c)); OPERATION; break; \
    case base + 0x1D: a = a_absi(c, c->x, &cross); m = RD(a); OPERATION; cyc += cross; break; \
    case base + 0x19: a = a_absi(c, c->y, &cross); m = RD(a); OPERATION; cyc += cross; break; \
    case base + 0x01: m = RD(a_indx(c)); OPERATION; break; \
    case base + 0x11: a = a_indy(c, &cross); m = RD(a); OPERATION; cyc += cross; break;

    ALU(0x00, (c->a |= m, setzn(c, c->a)))            /* ORA */
    ALU(0x20, (c->a &= m, setzn(c, c->a)))            /* AND */
    ALU(0x40, (c->a ^= m, setzn(c, c->a)))            /* EOR */
    ALU(0x60, op_adc(c, m))                           /* ADC */
    ALU(0xC0, op_cmp(c, c->a, m))                     /* CMP */
    ALU(0xE0, op_adc(c, (uint8_t)~m))                 /* SBC */
#undef ALU
    case 0xEB: m = fetch(c); op_adc(c, (uint8_t)~m); break; /* неофиц. SBC */

    /* --- CPX / CPY / BIT --- */
    case 0xE0: op_cmp(c, c->x, fetch(c)); break;
    case 0xE4: op_cmp(c, c->x, RD(a_zp(c))); break;
    case 0xEC: op_cmp(c, c->x, RD(a_abs(c))); break;
    case 0xC0: op_cmp(c, c->y, fetch(c)); break;
    case 0xC4: op_cmp(c, c->y, RD(a_zp(c))); break;
    case 0xCC: op_cmp(c, c->y, RD(a_abs(c))); break;
    case 0x24: case 0x2C:
        m = RD(op == 0x24 ? a_zp(c) : a_abs(c));
        c->p = (uint8_t)((c->p & ~(FLAG_Z | FLAG_V | FLAG_N)) | (m & 0xC0) | ((c->a & m) ? 0 : FLAG_Z));
        break;

#define RMW(base, FN) \
    case base + 0x06: a = a_zp(c);  WR(a, FN(c, RD(a))); break; \
    case base + 0x16: a = a_zpx(c); WR(a, FN(c, RD(a))); break; \
    case base + 0x0E: a = a_abs(c); WR(a, FN(c, RD(a))); break; \
    case base + 0x1E: a = a_absi(c, c->x, &cross); WR(a, FN(c, RD(a))); break;
    RMW(0x00, op_asl)
    RMW(0x20, op_rol)
    RMW(0x40, op_lsr)
    RMW(0x60, op_ror)
#undef RMW
    case 0x0A: c->a = op_asl(c, c->a); break;
    case 0x2A: c->a = op_rol(c, c->a); break;
    case 0x4A: c->a = op_lsr(c, c->a); break;
    case 0x6A: c->a = op_ror(c, c->a); break;

    /* --- INC / DEC --- */
    case 0xE6: a = a_zp(c);  m = (uint8_t)(RD(a) + 1); WR(a, m); setzn(c, m); break;
    case 0xF6: a = a_zpx(c); m = (uint8_t)(RD(a) + 1); WR(a, m); setzn(c, m); break;
    case 0xEE: a = a_abs(c); m = (uint8_t)(RD(a) + 1); WR(a, m); setzn(c, m); break;
    case 0xFE: a = a_absi(c, c->x, &cross); m = (uint8_t)(RD(a) + 1); WR(a, m); setzn(c, m); break;
    case 0xC6: a = a_zp(c);  m = (uint8_t)(RD(a) - 1); WR(a, m); setzn(c, m); break;
    case 0xD6: a = a_zpx(c); m = (uint8_t)(RD(a) - 1); WR(a, m); setzn(c, m); break;
    case 0xCE: a = a_abs(c); m = (uint8_t)(RD(a) - 1); WR(a, m); setzn(c, m); break;
    case 0xDE: a = a_absi(c, c->x, &cross); m = (uint8_t)(RD(a) - 1); WR(a, m); setzn(c, m); break;
    case 0xE8: c->x++; setzn(c, c->x); break;
    case 0xC8: c->y++; setzn(c, c->y); break;
    case 0xCA: c->x--; setzn(c, c->x); break;
    case 0x88: c->y--; setzn(c, c->y); break;

    /* --- переходы --- */
    case 0x4C: c->pc = fetch16(c); break;
    case 0x6C: {  /* JMP (abs) с ошибкой оригинального 6502 на границе страницы */
        uint16_t p = fetch16(c);
        c->pc = (uint16_t)(RD(p) | (RD((p & 0xFF00) | (uint8_t)(p + 1)) << 8));
        break;
    }
    case 0x20: {
        uint16_t to = fetch16(c);
        uint16_t ret = (uint16_t)(c->pc - 1);
        cpu_push(c, (uint8_t)(ret >> 8)); cpu_push(c, (uint8_t)ret);
        c->pc = to;
        break;
    }
    case 0x60: { uint8_t lo = cpu_pull(c); c->pc = (uint16_t)((lo | (cpu_pull(c) << 8)) + 1); break; }
    case 0x40: {
        c->p = (uint8_t)((cpu_pull(c) & ~FLAG_B) | FLAG_U);
        uint8_t lo = cpu_pull(c); c->pc = (uint16_t)(lo | (cpu_pull(c) << 8));
        break;
    }
    case 0x00: c->pc++; c->cycles += 7; return interrupt(c, 0xFFFE, 1);

    case 0x10: cyc += branch(c, !(c->p & FLAG_N)); break;
    case 0x30: cyc += branch(c,  (c->p & FLAG_N)); break;
    case 0x50: cyc += branch(c, !(c->p & FLAG_V)); break;
    case 0x70: cyc += branch(c,  (c->p & FLAG_V)); break;
    case 0x90: cyc += branch(c, !(c->p & FLAG_C)); break;
    case 0xB0: cyc += branch(c,  (c->p & FLAG_C)); break;
    case 0xD0: cyc += branch(c, !(c->p & FLAG_Z)); break;
    case 0xF0: cyc += branch(c,  (c->p & FLAG_Z)); break;

    /* --- флаги --- */
    case 0x18: c->p &= (uint8_t)~FLAG_C; break;
    case 0x38: c->p |= FLAG_C; break;
    case 0x58: c->p &= (uint8_t)~FLAG_I; break;
    case 0x78: c->p |= FLAG_I; break;
    case 0xB8: c->p &= (uint8_t)~FLAG_V; break;
    case 0xD8: c->p &= (uint8_t)~FLAG_D; break;
    case 0xF8: c->p |= FLAG_D; break;

    /* --- NOP, включая неофициальные --- */
    case 0xEA: case 0x1A: case 0x3A: case 0x5A: case 0x7A: case 0xDA: case 0xFA: break;
    case 0x80: case 0x82: case 0x89: case 0xC2: case 0xE2: fetch(c); break;
    case 0x04: case 0x44: case 0x64: fetch(c); break;
    case 0x14: case 0x34: case 0x54: case 0x74: case 0xD4: case 0xF4: fetch(c); break;
    case 0x0C: fetch16(c); break;
    case 0x1C: case 0x3C: case 0x5C: case 0x7C: case 0xDC: case 0xFC:
        a_absi(c, c->x, &cross); cyc += cross; break;

    /* --- неофициальные, которые иногда встречаются в играх --- */
    case 0xA7: c->a = c->x = RD(a_zp(c)); setzn(c, c->a); break;              /* LAX */
    case 0xAF: c->a = c->x = RD(a_abs(c)); setzn(c, c->a); break;
    case 0x87: WR(a_zp(c), c->a & c->x); break;                                /* SAX */
    case 0x8F: WR(a_abs(c), c->a & c->x); break;
    case 0xC7: a = a_zp(c);  m = (uint8_t)(RD(a) - 1); WR(a, m); op_cmp(c, c->a, m); break; /* DCP */
    case 0xCF: a = a_abs(c); m = (uint8_t)(RD(a) - 1); WR(a, m); op_cmp(c, c->a, m); break;
    case 0xE7: a = a_zp(c);  m = (uint8_t)(RD(a) + 1); WR(a, m); op_adc(c, (uint8_t)~m); break; /* ISB */
    case 0xEF: a = a_abs(c); m = (uint8_t)(RD(a) + 1); WR(a, m); op_adc(c, (uint8_t)~m); break;

    default:
        /* Неизвестная инструкция: останавливаемся, чтобы тест сразу это показал. */
        c->jammed = 1;
        c->pc--;
        break;
    }
#undef READ_OP
    c->cycles += (uint64_t)cyc;
    return cyc;
}
