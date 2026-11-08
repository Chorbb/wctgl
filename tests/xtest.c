/* xtest.c — проверка переведённого кода против оригинала.
 *
 *   xtest coverage rom.nes run.bin кадров out.txt   — какие инструкции исполнялись (для переводчика)
 *   xtest check    rom.nes run.bin кадров           — каждая переведённая процедура при каждом вызове
 *                                                     сверяется с оригиналом, исполненным на копии приставки
 *   xtest game     rom.nes run.bin кадров           — две приставки рядом: оригинал и порт;
 *                                                     после каждого кадра сравнивается всё состояние
 *
 * Эталон — наш же эмулятор, исполняющий оригинальный код. Перевод обязан совпасть с ним
 * до такта: память, регистры, видеочип, звук, номер такта.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../core/nes.h"
#include "../core/xlat.h"
#include "reclog.h"
#include "snaplog.h"
#include "../core/custom.h"

/* Источник: запись tgl_logger3/4 (одна последовательность от перезагрузки) или
 * tgl_logger5 (много отрезков, каждый — от своего снимка). */
static RecLog rl;
static SnapLog sl;
static int use_snaps;
static const Snap *cur_snap;
static const uint8_t *inp; static uint64_t inp_len, inp_start;
static uint64_t frame_base;

static void apply_input(Nes *n) {
    uint64_t k = inp_start + (n->ppu.frames_done - frame_base);
    n->pad_state = k < inp_len ? inp[k] : 0;
}

/* Приставка в начале текущего отрезка. Объект переиспользуется между отрезками
 * (таблица перехватов большая), поэтому ROM грузится один раз. */
static Nes *make_nes_into(Nes *n, const uint8_t *rom, long size) {
    if (!n) {
        n = calloc(1, sizeof(Nes));
        if (nes_load_rom(n, rom, (int)size)) { fprintf(stderr, "rom load error\n"); exit(2); }
        custom_install(n);           /* новые враги модов (без мода не задействованы) */
    }
    if (cur_snap) {
        snap_apply(n, cur_snap);
        inp = sl.input; inp_len = sl.frames + 1; inp_start = cur_snap->frame + 1;
    } else {
        memcpy(n->ram, rl.ram0, 2048);
        nes_reset(n);
        inp = rl.input; inp_len = (uint64_t)rl.frames; inp_start = 0;
    }
    n->frame = n->ppu.frames_done;
    n->hook_depth = 0; n->xlat_bail = 0; n->xlat_unwind = 0;
    frame_base = n->ppu.frames_done;
    n->frame_end_fn = apply_input;
    apply_input(n);
    return n;
}
static Nes *make_nes(const uint8_t *rom, long size) { return make_nes_into(NULL, rom, size); }

/* ---------------------------------------------------------------- сравнение состояний */

static int diff_state(const Nes *a, const Nes *b, char *why, int why_len) {
    const Cpu *x = &a->cpu, *y = &b->cpu;
    if (x->a != y->a || x->x != y->x || x->y != y->y || x->s != y->s || x->p != y->p || x->pc != y->pc) {
        snprintf(why, why_len, "регистры: A%02X/%02X X%02X/%02X Y%02X/%02X S%02X/%02X P%02X/%02X PC%04X/%04X",
                 x->a, y->a, x->x, y->x, x->y, y->y, x->s, y->s, x->p, y->p, x->pc, y->pc);
        return 1;
    }
    if (x->cycles != y->cycles) {
        snprintf(why, why_len, "такт: %llu/%llu (%+lld)", (unsigned long long)x->cycles, (unsigned long long)y->cycles,
                 (long long)(x->cycles - y->cycles));
        return 1;
    }
    if (x->nmi_level != y->nmi_level || x->irq_line != y->irq_line || x->need_nmi != y->need_nmi ||
        x->prev_need_nmi != y->prev_need_nmi || x->run_irq != y->run_irq || x->prev_run_irq != y->prev_run_irq ||
        x->prev_nmi_level != y->prev_nmi_level) { snprintf(why, why_len, "линии прерываний"); return 1; }
    for (int i = 0; i < 2048; i++)
        if (a->ram[i] != b->ram[i]) { snprintf(why, why_len, "память $%03X: %02X/%02X", i, a->ram[i], b->ram[i]); return 1; }
    static Ppu pa, pb;      /* frame_ended — учёт nes_run_frame, не состояние приставки */
    pa = a->ppu; pb = b->ppu; pa.frame_ended = pb.frame_ended = 0;
    if (memcmp(&pa, &pb, sizeof pa)) {
        const Ppu *p = &pa, *q = &pb;
        if (memcmp(p->framebuffer, q->framebuffer, sizeof p->framebuffer)) {
            int i = 0; while (p->framebuffer[i] == q->framebuffer[i]) i++;
            snprintf(why, why_len, "видеочип: картинка, строка %d", i / 256);
        } else if (memcmp(p->nt, q->nt, sizeof p->nt)) snprintf(why, why_len, "видеочип: фон");
        else if (memcmp(p->oam, q->oam, sizeof p->oam)) snprintf(why, why_len, "видеочип: спрайты");
        else snprintf(why, why_len, "видеочип: регистры v=%04X/%04X t=%04X/%04X ctrl=%02X/%02X mask=%02X/%02X st=%02X/%02X sl=%d/%d dot=%d/%d",
                      p->v, q->v, p->t, q->t, p->ctrl, q->ctrl, p->mask, q->mask, p->status, q->status,
                      p->scanline, q->scanline, p->dot, q->dot);
        return 1;
    }
    if (memcmp(&a->dmc, &b->dmc, sizeof a->dmc)) { snprintf(why, why_len, "канал DMC"); return 1; }
    if (a->bank != b->bank) { snprintf(why, why_len, "банк %d/%d", a->bank, b->bank); return 1; }
    if (memcmp(a->chr, b->chr, sizeof a->chr)) { snprintf(why, why_len, "CHR-RAM"); return 1; }
    if (a->master != b->master || a->pad_shift != b->pad_shift || a->pad_strobe != b->pad_strobe ||
        a->oam_dma_pending != b->oam_dma_pending || a->stall_cycles != b->stall_cycles) {
        snprintf(why, why_len, "шина/геймпад/DMA"); return 1;
    }
    return 0;
}

/* ---------------------------------------------------------------- покрытие */

static uint8_t *cov;   /* [банк 0..7][адрес] */

/* куда приводили RTS: [банк][адрес] — входы, в которые попадают не через JSR
 * (переходы по таблице через RTS, продолжение после параметров вызова) */
static uint8_t *rts_tgt;
static int last_was_rts;
static void trace_cov(Nes *n) {
    {
        uint16_t p0 = n->cpu.pc;
        int b0 = p0 >= 0xC000 ? 7 : (p0 >= 0x8000 ? (n->bank & 7) : -1);
        if (last_was_rts && b0 >= 0) rts_tgt[b0 * 0x10000 + p0] = 1;
        last_was_rts = nes_read(n, p0) == 0x60;
    }
    uint16_t pc = n->cpu.pc;
    int b = pc >= 0xC000 ? 7 : (pc >= 0x8000 ? (n->bank & 7) : -1);
    if (b >= 0) cov[b * 0x10000 + pc] = 1;
}

static void trace_prof(Nes *n) {
    /* XT из переведённого кода тоже зовёт trace_fn — считаем только эмулятор */
    static uint64_t last;
    if (n->emu_steps == last) return;
    last = n->emu_steps;
    uint16_t pc = n->cpu.pc;
    int b = pc >= 0xC000 ? 7 : (pc >= 0x8000 ? (n->bank & 7) : -1);
    if (b >= 0) ((uint32_t *)cov)[b * 0x10000 + pc]++;
}

/* statwatch: предыдущая инструкция (для источника значения) */
static FILE *stat_out;
static uint16_t prev_pc; static uint8_t prev_y, prev_x; static int prev_bank;
static void trace_stat(Nes *n) {
    uint16_t pc = n->cpu.pc;
    uint8_t op = nes_read(n, pc), lo = nes_read(n, (uint16_t)(pc + 1)), hi = nes_read(n, (uint16_t)(pc + 2));
    {
        static int watch = -2;
        if (watch == -2) watch = getenv("XT_PC") ? (int)strtol(getenv("XT_PC"), NULL, 16) : -1;
        if (watch >= 0 && pc == watch) {
            fprintf(stat_out, "H %04X %d %d %d", pc, n->bank & 7, n->ram[0x51], n->ram[0xB0] | (n->ram[0xB1] << 8));
            for (int k = 0; k < 16; k++) fprintf(stat_out, " %02X", n->ram[0x4C0 + k]);
            fprintf(stat_out, "\n");
            return;
        }
        if (watch >= 0) return;
    }
    if (getenv("XT_ROOM")) {
        /* комната лабиринта построена: возвраты из $8003 (банк 5) в $EBC3/$EE83/$EF00/$FF71.
         * Пишем: "M" x y зона | адрес записи комнаты | 192 клетки $740-$7FF | наборы $460-$477 |
         * палитра 32 | CHR (8 КБ, только когда изменилась — "K") */
        if (pc == 0xEBC3 || pc == 0xEE83 || pc == 0xEF00 || pc == 0xFF71) {
            static uint32_t last_chr_hash; uint32_t h = 2166136261u;
            for (int i = 0; i < 8192; i++) { h ^= n->chr[i]; h *= 16777619u; }
            if (h != last_chr_hash) {
                fprintf(stat_out, "K %08X ", h);
                for (int i = 0; i < 8192; i++) fprintf(stat_out, "%02X", n->chr[i]);
                fprintf(stat_out, "\n");
                last_chr_hash = h;
            }
            fprintf(stat_out, "M %d %d %d %04X %08X %02X ", n->ram[0x52], n->ram[0x53], n->ram[0x50],
                    n->ram[0xE4] | (n->ram[0xE5] << 8), h, n->ppu.ctrl);
            for (int i = 0; i < 192; i++) fprintf(stat_out, "%02X", n->ram[0x740 + i]);
            fprintf(stat_out, " ");
            for (int i = 0; i < 24; i++) fprintf(stat_out, "%02X", n->ram[0x460 + i]);
            fprintf(stat_out, " ");
            for (int i = 0; i < 32; i++) fprintf(stat_out, "%02X", n->ppu.pal[i]);
            fprintf(stat_out, " %04X\n", pc);
        }
        return;
    }
    if (getenv("XT_CORR")) {
        /* поток данных коридора: начало ($E58F, указатель $18) и каждая команда ($E6F0) */
        if (pc == 0xE58F)
            fprintf(stat_out, "B %04X %d %d %d\n", n->ram[0x18] | (n->ram[0x19] << 8), n->bank & 7, n->ram[0x51], n->ram[0x50]);
        if (pc == 0xE974) {                        /* ряд клеток готов (фон + кисти) */
            fprintf(stat_out, "W %d", n->ram[0xB0] | (n->ram[0xB1] << 8));
            for (int k = 0; k < 16; k++) fprintf(stat_out, " %02X", n->ram[0x4C0 + k]);
            fprintf(stat_out, "\n");
        }
        if (pc == 0xE6F0)
            fprintf(stat_out, "C %d %04X %04X %d\n", n->cpu.a, (uint16_t)((n->ram[0xBC] | (n->ram[0xBD] << 8)) + n->cpu.y - 1),
                    n->ram[0xB0] | (n->ram[0xB1] << 8), n->bank & 7);
        return;
    }
    if (getenv("XT_SCRIPT")) {
        /* начало сценария волн: после $E75B указатель $66/$67 и банк $7F заданы */
        if (pc == 0xE75D)
            fprintf(stat_out, "P %04X %d %d %d %d\n", n->ram[0x66] | (n->ram[0x67] << 8), n->ram[0x7F],
                    n->ram[0x50], n->ram[0x52], n->ram[0x53]);
        if (pc == 0xA48A && (n->bank & 7) == 5)     /* сценарий комнаты лабиринта: таблица $A4C5 по $57 */
            fprintf(stat_out, "P %04X %d %d %d %d\n", n->ram[0x66] | (n->ram[0x67] << 8), 5,
                    n->ram[0x50], n->ram[0x52], n->ram[0x53]);
        if (pc == 0xF828) fprintf(stat_out, "E %d\n", n->cpu.a);                       /* враг появился */
        if (pc == 0xF840) fprintf(stat_out, "G %d\n", n->ram[0x548 + n->cpu.x]);       /* генератор */
        if (pc == 0xF7FC) fprintf(stat_out, "F %d\n", n->ram[0x16]);                   /* не нашлось места */
        return;
    }
    if (getenv("XT_SPAWN2")) {
        /* любое появление: ячейка $500+k стала ненулевой — виновата предыдущая инструкция */
        static uint8_t last[24];
        for (int k = 0; k < 24; k++) {
            uint8_t v = n->ram[0x500 + k];
            if (v && !last[k]) {
                fprintf(stat_out, "N %04X %d %02X %d %d %d\n", prev_pc, prev_bank, v, k, n->ram[0x50],
                        n->ram[0x52] | (n->ram[0x53] << 8));
            }
            last[k] = v;
        }
        prev_pc = pc; prev_y = n->cpu.y; prev_x = n->cpu.x;
        prev_bank = pc >= 0xC000 ? 7 : (n->bank & 7);
        return;
    }
    if (getenv("XT_SPAWN")) {
        /* появление объектов: запись ненулевого типа в $500,X (STA abs,X / abs,Y) */
        if ((op == 0x9D || op == 0x99) && hi == 0x05 && lo == 0x00 && n->cpu.a) {
            uint8_t pop = nes_read(n, prev_pc);
            uint16_t src = (uint16_t)(nes_read(n, (uint16_t)(prev_pc + 1)) | (nes_read(n, (uint16_t)(prev_pc + 2)) << 8));
            int idx = -1;
            if (pop == 0xB9) idx = prev_y; else if (pop == 0xBD) idx = prev_x;
            else if (pop == 0xB1) {                        /* LDA (zp),Y: адрес = указатель + Y */
                uint8_t z = nes_read(n, (uint16_t)(prev_pc + 1));
                src = (uint16_t)(n->ram[z] | (n->ram[(uint8_t)(z + 1)] << 8)); idx = prev_y;
            } else if (pop == 0xA9) { src = 0xFFFF; idx = nes_read(n, (uint16_t)(prev_pc + 1)); }
            else src = 0;
            int bank = pc >= 0xC000 ? 7 : (n->bank & 7);
            fprintf(stat_out, "S %04X %d %02X %02X %04X %d %d %d %d\n", pc, bank, n->cpu.a, pop, src, idx,
                    prev_bank, n->ram[0x50], n->ram[0x52] | (n->ram[0x53] << 8));
        }
        prev_pc = pc; prev_y = n->cpu.y; prev_x = n->cpu.x;
        prev_bank = pc >= 0xC000 ? 7 : (n->bank & 7);
        return;
    }
    if (op == 0x9D && hi == 0x05 && (lo == 0x90 || lo == 0xA8)) {
        uint8_t pop = nes_read(n, prev_pc);
        uint16_t src = (uint16_t)(nes_read(n, (uint16_t)(prev_pc + 1)) | (nes_read(n, (uint16_t)(prev_pc + 2)) << 8));
        int idx = -1;
        if (pop == 0xB9) idx = prev_y; else if (pop == 0xBD) idx = prev_x; else if (pop == 0xAD) idx = 0;
        else src = 0;
        if (pop == 0xA9) { src = 0xFFFF; idx = nes_read(n, (uint16_t)(prev_pc + 1)); }   /* константа */
        int bank = pc >= 0xC000 ? 7 : (n->bank & 7);
        fprintf(stat_out, "W %s %04X %d %02X %02X %04X %d %d\n", lo == 0x90 ? "hp" : "dmg", pc, bank,
                n->ram[0x500 + n->cpu.x], n->cpu.a, src, idx, prev_bank);
    }
    prev_pc = pc; prev_y = n->cpu.y; prev_x = n->cpu.x;
    prev_bank = pc >= 0xC000 ? 7 : (n->bank & 7);
}

/* ---------------------------------------------------------------- проверка процедур */

/* трасса: состояние перед каждой инструкцией */
typedef struct { uint16_t pc; uint8_t a, x, y, s, p; uint64_t cyc; } Tr;
static Tr *tr_buf; static int tr_n, tr_cap = 1 << 20;
static void trace_rec(Nes *n) {
    if (tr_n < tr_cap) tr_buf[tr_n++] = (Tr){ n->cpu.pc, n->cpu.a, n->cpu.x, n->cpu.y, n->cpu.s, n->cpu.p, n->cpu.cycles };
}
static void print_tr(const char *who, const Tr *t) {
    printf("    %s %04X A%02X X%02X Y%02X S%02X P%02X такт %llu\n", who, t->pc, t->a, t->x, t->y, t->s, t->p,
           (unsigned long long)t->cyc);
}

static int checking = 0;
static long checks = 0, check_fail = 0, spans = 0;
static long per_fn_calls[4096], per_fn_fail[4096];

static int find_info(Nes *n, uint16_t pc) {
    for (int i = 0; i < xlat_count; i++) {
        const XlatInfo *x = &xlat_table[i];
        if (x->entry != pc) continue;
        if (pc >= 0x8000 && pc < 0xC000 && x->bank != (n->bank & 7)) continue;
        return i;
    }
    return -1;
}

/* до возврата: стек поднялся до s_ret; для круга цикла (loop_pc) — до возврата к его началу */
static int loop_pc = -1;
/* процедура, не вернувшаяся за столько кадров (финальные титры, сброс стека), не сравнивается */
#define FINISH_MAX_FRAMES 30
static int finish_timeout;
static long check_skipped;
static void finish(Nes *n, uint8_t s_ret) {
    if (loop_pc >= 0) {
        uint16_t head = (uint16_t)loop_pc;
        loop_pc = -1;
        uint64_t f0 = n->ppu.frames_done;
        do nes_step(n); while (!(n->cpu.pc == head && n->cpu.s == s_ret) && !n->cpu.jammed &&
                               n->ppu.frames_done - f0 < FINISH_MAX_FRAMES);
        if (n->ppu.frames_done - f0 >= FINISH_MAX_FRAMES) finish_timeout = 1;
        return;
    }
    uint64_t f0 = n->ppu.frames_done;
    while ((uint8_t)(n->cpu.s - s_ret) >= 0x80 && !n->cpu.jammed && n->ppu.frames_done - f0 < FINISH_MAX_FRAMES)
        nes_step(n);
    if (n->ppu.frames_done - f0 >= FINISH_MAX_FRAMES) finish_timeout = 1;
}

static void finish_x(Nes *n, uint8_t s_ret, const XlatInfo *x) {
    /* перевод отдал управление прямо на входе: первую инструкцию исполняет эмулятор
     * (как в nes_step), иначе перехват на том же адресе вызовется снова */
    if (n->cpu.pc == x->entry && n->use_hooks) { n->emu_steps++; if (n->trace_fn) n->trace_fn(n); cpu_step(&n->cpu); }
    if (x->kind == 2) loop_pc = x->entry;
    finish(n, s_ret);
}

/* после перевода: если процедура не дошла до своего возврата (отдала управление или
 * свернулась по времени) — доисполнить эмулятором. Судим по стеку, а не по флагам. */
static void complete(Nes *n, uint8_t s_ret, const XlatInfo *x) {
    int bailed = n->xlat_bail;
    n->xlat_bail = 0;
    if (n->xlat_unwind) { n->xlat_unwind = 0; n->hook_frames0 = n->ppu.frames_done; }
    int done = x->kind == 2 ? (n->cpu.pc == x->entry && n->cpu.s == s_ret && !bailed)
                            : (uint8_t)(n->cpu.s - s_ret) < 0x80;
    if (!done) finish_x(n, s_ret, x);
}

static void check_wrapper(Nes *n) {
    int i = find_info(n, n->cpu.pc);
    if (i < 0) { fprintf(stderr, "нет процедуры для $%04X\n", n->cpu.pc); exit(3); }
    const XlatInfo *x = &xlat_table[i];
    uint8_t s_ret = (uint8_t)(n->cpu.s + (x->kind == 1 ? 3 : x->kind == 0 ? 2 : 0));
    if (checking) {                 /* вложенный вызов внутри проверки: просто перевод */
        x->fn(n);
        /* свёртка по времени — дело внешнего уровня: не сбрасывать её здесь, иначе
         * внешний цикл ожидания никогда не свернётся */
        if (n->xlat_unwind) return;
        complete(n, s_ret, x);
        return;
    }
    static Nes ref, *saved;
    if (!saved) saved = malloc(sizeof(Nes));
    *saved = *n;
    ref = *n;
    ref.cpu.ctx = &ref;
    ref.use_hooks = 0;
    uint64_t f0 = n->ppu.frames_done;
    finish_x(&ref, s_ret, x);

    checking = 1;
    x->fn(n);
    complete(n, s_ret, x);
    checking = 0;

    if (finish_timeout) {
        finish_timeout = 0; check_skipped++;
        *n = ref; n->cpu.ctx = n; n->use_hooks = 1;   /* продолжаем с эталона */
        return;
    }
    checks++; per_fn_calls[i]++;
    if (n->ppu.frames_done != f0) spans++;
    char why[256];
    if (diff_state(n, &ref, why, sizeof why)) {
        check_fail++; per_fn_fail[i]++;
        if (per_fn_fail[i] <= 3) {
            printf("РАСХОЖДЕНИЕ %s (кадр %llu): перевод/оригинал — %s\n", x->name,
                   (unsigned long long)(n->ppu.frames_done - frame_base), why);
            if (getenv("XT_TRACE")) {
                /* повторить обе стороны с трассой и показать первое расхождение */
                static Nes a, b; static Tr *ta, *tb; int na, nb;
                if (!tr_buf) tr_buf = malloc(sizeof(Tr) * (size_t)tr_cap);
                a = *saved; a.cpu.ctx = &a; a.use_hooks = 0; a.trace_fn = trace_rec;
                tr_n = 0; finish_x(&a, s_ret, x); na = tr_n;
                ta = malloc(sizeof(Tr) * (size_t)na); memcpy(ta, tr_buf, sizeof(Tr) * (size_t)na);
                b = *saved; b.cpu.ctx = &b; b.trace_fn = trace_rec;
                tr_n = 0; checking = 1; x->fn(&b); complete(&b, s_ret, x); checking = 0;
                nb = tr_n; tb = tr_buf;
                int k = 0;
                while (k < na && k < nb && !memcmp(&ta[k], &tb[k], sizeof(Tr))) k++;
                printf("    инструкций: оригинал %d, перевод %d; первое расхождение на #%d\n", na, nb, k);
                for (int j = k - 3 < 0 ? 0 : k - 3; j <= k + 1; j++) {
                    if (j < na) print_tr("ориг", &ta[j]);
                    if (j < nb) print_tr("пер ", &tb[j]);
                }
                free(ta);
            }
        }
        /* продолжаем с оригинального состояния, чтобы одна ошибка не тянула за собой остальные */
        *n = ref;
        n->cpu.ctx = n;
        n->use_hooks = 1;
    }
}

/* ---------------------------------------------------------------- доля перевода */

static uint64_t emu_bytes_exec, emu_instr;
static void trace_count(Nes *n) { (void)n; emu_instr++; }

int main(int argc, char **argv) {
    if (argc < 5) {
        fprintf(stderr, "usage: xtest coverage|check|game|profile rom.nes run.bin|snap.bin frames [out.txt]\n"
                        "  snap.bin: каждый снимок — отдельный отрезок длиной frames кадров;\n"
                        "  XT_SNAPS=a:b — только снимки с a по b-1, XT_SNAP_STEP=k — каждый k-й\n");
        return 2;
    }
    const char *mode = argv[1];
    long rom_size;
    uint8_t *rom = rl_read_file(argv[2], &rom_size);
    if (!rom) { fprintf(stderr, "cannot read rom\n"); return 2; }
    if (getenv("XT_PATCH")) {
        /* мод: файл .tglpatch (tools/mod.py) — строки «банк адрес байты…» прямо в копию ROM */
        FILE *pf = fopen(getenv("XT_PATCH"), "r");
        if (!pf) { fprintf(stderr, "cannot read patch\n"); return 2; }
        char line[1024]; int np = 0, nbank = (rom[4] ? rom[4] : 8);
        while (fgets(line, sizeof line, pf)) {
            if (line[0] == '#' || line[0] == 't' || line[0] == '\n') continue;
            char *q = line; int bank = (int)strtol(q, &q, 10); int addr = (int)strtol(q, &q, 16);
            long off = 16 + (long)bank * 0x4000 + (addr - (bank == nbank - 1 ? 0xC000 : 0x8000));
            for (;;) { char *q2; long v = strtol(q, &q2, 16); if (q2 == q) break; q = q2;
                       if (off >= 16 && off < rom_size) rom[off] = (uint8_t)v; off++; np++; }
        }
        fclose(pf);
        fprintf(stderr, "patch: %d bytes\n", np);
    }
    int frames = atoi(argv[4]);
    int s_from = 0, s_to = 1, s_step = 1;
    int e = snaplog_load(&sl, argv[3]);
    if (e == 0) {
        use_snaps = 1;
        s_to = sl.nsnaps;
        const char *r = getenv("XT_SNAPS");
        if (r) { sscanf(r, "%d:%d", &s_from, &s_to); if (s_to > sl.nsnaps) s_to = sl.nsnaps; }
        if (getenv("XT_SNAP_STEP")) s_step = atoi(getenv("XT_SNAP_STEP"));
    } else if (e == -2) {
        if (reclog_load(&rl, argv[3])) { fprintf(stderr, "cannot read recording\n"); return 2; }
        if (frames > rl.frames) frames = rl.frames;
    } else { fprintf(stderr, "cannot read snapshot file (%d)\n", e); return 2; }

    if (!strcmp(mode, "dump")) {
        /* для разметки памяти: каждые XT_EVERY кадров — память в конце кадра, экраны (2 КБ),
         * спрайты (256 Б) и кнопки. Запись: "D" | отрезок(2) | кадр(2) | кнопки(1) | RAM | NT | OAM */
        if (!use_snaps) { fprintf(stderr, "dump: нужен snap.bin\n"); return 2; }
        int every = getenv("XT_EVERY") ? atoi(getenv("XT_EVERY")) : 4;
        FILE *o = fopen(argc > 5 ? argv[5] : "build/dump.bin", "wb");
        Nes *m = NULL; long recs = 0;
        for (int si = s_from; si < s_to; si += s_step) {
            cur_snap = &sl.snaps[si];
            m = make_nes_into(m, rom, rom_size);
            xlat_register_all(m); m->use_hooks = 1;
            int segf = frames;
            if (cur_snap->frame + 1 + (uint32_t)segf > sl.frames) segf = (int)(sl.frames - cur_snap->frame - 1);
            for (int f = 0; f < segf; f++) {
                nes_run_frame(m);
                if (f % every) continue;
                uint8_t h[6] = { 'D', (uint8_t)si, (uint8_t)(si >> 8), (uint8_t)f, (uint8_t)(f >> 8), m->pad_state };
                fwrite(h, 1, 6, o); fwrite(m->fe_ram, 1, 2048, o); fwrite(m->ppu.nt, 1, 2048, o); fwrite(m->ppu.oam, 1, 256, o);
                recs++;
            }
        }
        fclose(o);
        printf("dump: %ld records\n", recs);
        return 0;
    }

    if (!strcmp(mode, "statwatch")) {
        /* откуда объекты получают здоровье и урон: при каждой записи STA $0590,X / STA $05A8,X
         * запоминается тип объекта, значение и источник (предыдущая инструкция LDA таблица,Y).
         * Вывод: "W массив pc банк тип значение источник индекс" (уникальные строки). */
        if (!use_snaps) { fprintf(stderr, "statwatch: нужен snap.bin\n"); return 2; }
        FILE *o = fopen(argc > 5 ? argv[5] : "build/statwatch.txt", "w");
        Nes *m = NULL;
        for (int si = s_from; si < s_to; si += s_step) {
            cur_snap = &sl.snaps[si];
            m = make_nes_into(m, rom, rom_size);
            m->use_hooks = 0; m->trace_fn = trace_stat; stat_out = o;
            fprintf(o, "R %d\n", si);                 /* начало отрезка: прежний сценарий не продолжается */
            int segf = frames;
            if (cur_snap->frame + 1 + (uint32_t)segf > sl.frames) segf = (int)(sl.frames - cur_snap->frame - 1);
            for (int f = 0; f < segf; f++) nes_run_frame(m);
        }
        fclose(o);
        printf("statwatch: done\n");
        return 0;
    }

    if (!strcmp(mode, "statedump")) {
        /* для картинок уровня: в кадре 300 отрезка — память, CHR, палитра, PPUCTRL.
         * Запись: "T" | отрезок(2) | коридор $51 | путь в кадре 290 (2) | путь в кадре 300 (2) | ctrl | RAM | CHR | PAL */
        if (!use_snaps) { fprintf(stderr, "statedump: нужен snap.bin\n"); return 2; }
        FILE *o = fopen(argc > 5 ? argv[5] : "build/states.bin", "wb");
        Nes *m = NULL; long recs = 0;
        for (int si = s_from; si < s_to; si += s_step) {
            cur_snap = &sl.snaps[si];
            m = make_nes_into(m, rom, rom_size);
            xlat_register_all(m); m->use_hooks = 1;
            if (cur_snap->frame + 302 > sl.frames) continue;
            uint16_t p290 = 0;
            for (int f = 0; f <= 300; f++) {
                nes_run_frame(m);
                if (f == 290) p290 = (uint16_t)(m->fe_ram[0xB0] | (m->fe_ram[0xB1] << 8));
            }
            uint16_t p300 = (uint16_t)(m->fe_ram[0xB0] | (m->fe_ram[0xB1] << 8));
            uint8_t h[9] = { 'T', (uint8_t)si, (uint8_t)(si >> 8), m->fe_ram[0x51], (uint8_t)p290, (uint8_t)(p290 >> 8),
                             (uint8_t)p300, (uint8_t)(p300 >> 8), m->ppu.ctrl };
            fwrite(h, 1, 9, o); fwrite(m->fe_ram, 1, 2048, o); fwrite(m->chr, 1, 8192, o); fwrite(m->ppu.pal, 1, 32, o);
            fwrite(m->shown, 1, sizeof m->shown, o);     /* как выглядел экран — для сверки картинок */
            recs++;
        }
        fclose(o);
        printf("statedump: %ld states\n", recs);
        return 0;
    }

    if (!strcmp(mode, "roomgen")) {
        /* все комнаты лабиринта, построенные процедурами самой игры: от снимка XT_SNAPS=a
         * (игрок в лабиринте) для каждой клетки сетки 24×24 с записью комнаты — X/Y в $52/$53,
         * зона из записи, затем $8071 (записи комнаты и соседей), $8121 (флаги), $8155 (постройка),
         * банк 5. Вывод — строки M, как у XT_ROOM (графика — та, что в снимке). */
        if (!use_snaps) { fprintf(stderr, "roomgen: нужен snap.bin\n"); return 2; }
        cur_snap = &sl.snaps[s_from];
        Nes *base = make_nes_into(NULL, rom, rom_size);
        base->use_hooks = 0;
        for (int f = 0; f < frames; f++) nes_run_frame(base);
        FILE *o = fopen(argc > 5 ? argv[5] : "build/roomgen.txt", "w");
        static Nes w;
        /* записи комнат: банк 5, $8A6E, 24×24 подряд, длина = (байт0 & $0F) + 1 */
        const uint8_t *prg5 = base->prg + 5 * 0x4000;
        uint16_t p = 0x8A6E;
        int made = 0;
        for (int y = 0; y < 24; y++)
            for (int x = 0; x < 24; x++) {
                uint8_t b0 = prg5[p - 0x8000];
                int len = (b0 & 0x0F) + 1;
                uint16_t rec = p;
                p = (uint16_t)(p + len);
                if (b0 == 0x80) continue;                     /* комнаты нет */
                /* зона: у записей с битом 7 — 0 (центр), иначе младшие 4 бита байта 2 (по записи) */
                int area = (b0 & 0x80) || len < 3 ? 0 : (prg5[rec + 2 - 0x8000] & 0x0F);
                w = *base; w.cpu.ctx = &w; w.frame_end_fn = NULL;
                w.ram[0x52] = (uint8_t)x; w.ram[0x53] = (uint8_t)y; w.ram[0xA4] = 4; w.ram[0x50] = (uint8_t)area;
                w.ram[0x30] &= (uint8_t)~0x60;              /* не у ворот коридора: шаблоны лабиринта ($A749) */
                w.bank = 5; w.ram[0x04] = 5;
                memset(w.ram + 0x740, 0, 192);
                static const uint16_t steps[3] = { 0x8071, 0x8121, 0x8158 };   /* $8155 без JSR $C045 (выключение экрана и ожидание кадров) */
                int ok = 1;
                for (int k = 0; k < 3 && ok; k++) {
                    uint8_t s0 = w.cpu.s;
                    uint16_t ret = 0xFFF0;                     /* возврат в «никуда» — ловим по адресу */
                    w.ram[0x100 | w.cpu.s] = (uint8_t)((ret - 1) >> 8); w.cpu.s--;
                    w.ram[0x100 | w.cpu.s] = (uint8_t)(ret - 1); w.cpu.s--;
                    w.cpu.pc = steps[k];
                    long guard = 0;
                    while (!(w.cpu.pc == ret && w.cpu.s == s0) && guard++ < 300000 && !w.cpu.jammed) nes_step(&w);
                    if (w.cpu.pc != ret) { ok = 0; fprintf(o, "# step %04X stuck\n", steps[k]); }
                    w.bank = 5;
                }
                /* у записей с битом 7 (особые комнаты: вход в коридор, магазин и т. п.) игра строит
                 * не комнату карты, а её «внутренность»: код банка 7 с $EE1D выбирает вид ($51) по
                 * флагам прогресса $04A0/$04A9, грузит графику ($8000), ставит $50=0, $30|=$20 и
                 * строит ($8003). Исполняем его как есть, до возврата в $EE83. */
                if (ok && (b0 & 0x80)) {
                    memset(w.ram + 0x740, 0, 192);
                    w.cpu.pc = 0xEE1D;
                    long guard = 0;
                    while (w.cpu.pc != 0xEE83 && guard++ < 3000000 && !w.cpu.jammed) nes_step(&w);
                    if (w.cpu.pc != 0xEE83) { ok = 0; fprintf(o, "# special stuck\n"); }
                    area = w.ram[0x51] | 0x100;                /* в выводе: 256 + вид особой комнаты */
                }
                if (!ok) { fprintf(o, "X %d %d pc %04X bank %d\n", x, y, w.cpu.pc, w.bank); continue; }
                {
                    static uint32_t last_h; uint32_t h = 2166136261u;
                    for (int i = 0; i < 8192; i++) { h ^= w.chr[i]; h *= 16777619u; }
                    if (h != last_h) {
                        fprintf(o, "K %08X ", h);
                        for (int i = 0; i < 8192; i++) fprintf(o, "%02X", w.chr[i]);
                        fprintf(o, "\n");
                        last_h = h;
                    }
                    fprintf(o, "M %d %d %d %04X %08X %02X ", x, y, area, rec, h, w.ppu.ctrl);
                }
                for (int i = 0; i < 192; i++) fprintf(o, "%02X", w.ram[0x740 + i]);
                fprintf(o, " ");
                for (int i = 0; i < 24; i++) fprintf(o, "%02X", w.ram[0x460 + i]);
                fprintf(o, " ");
                for (int i = 0; i < 32; i++) fprintf(o, "%02X", w.ppu.pal[i]);
                fprintf(o, " GEN\n");
                made++;
            }
        fclose(o);
        printf("roomgen: %d rooms, map data ends at $%04X\n", made, p);
        return 0;
    }

    if (!strcmp(mode, "wav")) {
        /* звук: от снимка XT_SNAPS=a (по умолчанию 1) frames кадров в WAV 48 кГц моно */
        if (!use_snaps) { fprintf(stderr, "wav: нужен snap.bin\n"); return 2; }
        cur_snap = &sl.snaps[s_from < sl.nsnaps ? s_from : 0];
        Nes *m = make_nes_into(NULL, rom, rom_size);
        xlat_register_all(m); m->use_hooks = 1;
        FILE *o = fopen(argc > 5 ? argv[5] : "build/out.wav", "wb");
        uint32_t ns = 0;
        fwrite("RIFF\0\0\0\0WAVEfmt \x10\0\0\0\x01\0\x01\0\x80\xbb\0\0\0\x77\x01\0\x02\0\x10\0data\0\0\0\0", 1, 44, o);
        int16_t buf[4096];
        for (int f = 0; f < frames; f++) {
            nes_run_frame(m);
            int k;
            while ((k = apu_read(&m->apu, buf, 4096)) > 0) { fwrite(buf, 2, (size_t)k, o); ns += (uint32_t)k; }
        }
        uint32_t data = ns * 2, riff = 36 + data;
        fseek(o, 4, SEEK_SET); fwrite(&riff, 4, 1, o);
        fseek(o, 40, SEEK_SET); fwrite(&data, 4, 1, o);
        fclose(o);
        printf("wav: %u samples (%.1f s)\n", ns, ns / 48000.0);
        return 0;
    }

    if (!strcmp(mode, "apu")) {
        /* Звук против Mesen: от снимка k наш эмулятор идёт до момента снимка k+1, и состояние
         * звуковых каналов сравнивается с тем, что записал Mesen. Ход игры к тому времени
         * расходится, но музыка идёт по своей партитуре — совпадения много, где нет эффектов. */
        if (!use_snaps) { fprintf(stderr, "apu: нужен snap.bin\n"); return 2; }
        enum { F_P1PER, F_P1DUTY, F_P1VOL, F_P1LEN, F_P2PER, F_P2DUTY, F_P2VOL, F_P2LEN,
               F_TPER, F_TLIN, F_TLEN, F_NPER, F_NVOL, F_NLEN, F_N };
        static const char *names[F_N] = { "sq1 period", "sq1 duty", "sq1 volume", "sq1 length",
            "sq2 period", "sq2 duty", "sq2 volume", "sq2 length", "tri period", "tri linear", "tri length",
            "noise period", "noise volume", "noise length" };
        long same[F_N] = {0}, total = 0;
        Nes *m = NULL;
        for (int si = s_from; si + 1 < s_to; si += s_step) {
            const Snap *a = &sl.snaps[si], *b = &sl.snaps[si + 1];
            cur_snap = a;
            m = make_nes_into(m, rom, rom_size);
            m->use_hooks = 0; m->trace_fn = NULL;
            uint64_t need = b->frame - a->frame;
            while (m->ppu.frames_done - frame_base < need) nes_step(m);
            long guard = 0;
            while (m->cpu.pc != 0xDC75 && guard++ < 200000) nes_step(m);
            if (m->cpu.pc != 0xDC75) continue;
            Apu *u = &m->apu;
            long mv[F_N] = {
                snap_get(b, "apu.square1.realPeriod", -1), snap_get(b, "apu.square1.duty", -1),
                snap_get(b, "apu.square1.envelope.constantVolume", 0) ? snap_get(b, "apu.square1.envelope.volume", -1)
                                                                      : snap_get(b, "apu.square1.envelope.counter", -1),
                snap_get(b, "apu.square1.envelope.lengthCounter.counter", -1),
                snap_get(b, "apu.square2.realPeriod", -1), snap_get(b, "apu.square2.duty", -1),
                snap_get(b, "apu.square2.envelope.constantVolume", 0) ? snap_get(b, "apu.square2.envelope.volume", -1)
                                                                      : snap_get(b, "apu.square2.envelope.counter", -1),
                snap_get(b, "apu.square2.envelope.lengthCounter.counter", -1),
                snap_get(b, "apu.triangle.timer.period", -1), snap_get(b, "apu.triangle.linearCounter", -1),
                snap_get(b, "apu.triangle.lengthCounter.counter", -1),
                snap_get(b, "apu.noise.timer.period", -2) + 1,
                snap_get(b, "apu.noise.envelope.constantVolume", 0) ? snap_get(b, "apu.noise.envelope.volume", -1)
                                                                    : snap_get(b, "apu.noise.envelope.counter", -1),
                snap_get(b, "apu.noise.envelope.lengthCounter.counter", -1) };
            long ov[F_N] = { u->p1.period, u->p1.duty, u->p1.env.constant ? u->p1.env.volume : u->p1.env.decay, u->p1.length,
                             u->p2.period, u->p2.duty, u->p2.env.constant ? u->p2.env.volume : u->p2.env.decay, u->p2.length,
                             u->tri.period, u->tri.linear, u->tri.length,
                             u->noise.period, u->noise.env.constant ? u->noise.env.volume : u->noise.env.decay, u->noise.length };
            for (int f = 0; f < F_N; f++) same[f] += mv[f] == ov[f];
            total++;
            if (getenv("XT_VERBOSE")) {
                printf("снимок %d:", si + 1);
                for (int f = 0; f < F_N; f++) if (mv[f] != ov[f]) printf("  %s %ld/%ld", names[f], ov[f], mv[f]);
                printf("\n");
            }
        }
        printf("apu: %ld snapshots compared (ours/Mesen)\n", total);
        for (int f = 0; f < F_N; f++) printf("  %-13s %5.1f%% same\n", names[f], 100.0 * same[f] / (total ? total : 1));
        return 0;
    }

    int is_cov = !strcmp(mode, "coverage"), is_check = !strcmp(mode, "check");
    int is_prof = !strcmp(mode, "profile"), is_game = !strcmp(mode, "game");
    if (!is_cov && !is_check && !is_prof && !is_game) { fprintf(stderr, "unknown mode\n"); return 2; }
    if (is_cov) { cov = calloc(8, 0x10000); rts_tgt = calloc(8, 0x10000); }
    if (is_prof) cov = calloc(8 * 0x10000, sizeof(uint32_t));

    Nes *n = NULL, *ref = NULL;
    long total_frames = 0, bad = 0, segs = 0;
    uint64_t ref_steps = 0, port_steps = 0, unw = 0;
    for (int si = s_from; si < s_to; si += s_step) {
        cur_snap = use_snaps ? &sl.snaps[si] : NULL;
        int seg_frames = frames;
        if (use_snaps && cur_snap->frame + 1 + (uint32_t)seg_frames > sl.frames) seg_frames = (int)(sl.frames - cur_snap->frame - 1);
        if (seg_frames <= 0) continue;
        segs++;
        n = make_nes_into(n, rom, rom_size);
        if (is_cov) {
            n->use_hooks = 0; n->trace_fn = trace_cov;
            for (int f = 0; f < seg_frames; f++) nes_run_frame(n);
            if (getenv("XT_PPM")) {      /* картинка конца отрезка — проверить, что игра из снимка идёт как надо */
                char path[512]; snprintf(path, sizeof path, "%s/seg%03d.ppm", getenv("XT_PPM"), si);
                FILE *o = fopen(path, "wb");
                fprintf(o, "P6 256 240 255\n");
                for (int i = 0; i < 256 * 240; i++) {
                    uint32_t c = NES_PALETTE[n->shown[i] & 63];
                    fputc((c >> 16) & 255, o); fputc((c >> 8) & 255, o); fputc(c & 255, o);
                }
                fclose(o);
            }
        } else if (is_check) {
            for (int i = 0; i < xlat_count; i++)
                nes_set_hook(n, xlat_table[i].bank < 0 ? 7 : xlat_table[i].bank, xlat_table[i].entry, check_wrapper);
            n->use_hooks = 1;
            long before = check_fail;
            for (int f = 0; f < seg_frames; f++) nes_run_frame(n);
            if (check_fail != before && use_snaps) printf("  ^ отрезок от снимка %d (кадр записи %u)\n", si, cur_snap->frame);
        } else if (is_prof) {
            xlat_register_all(n); n->use_hooks = 1; n->trace_fn = trace_prof;
            for (int f = 0; f < seg_frames; f++) nes_run_frame(n);
        } else {
            /* game: эталон (только эмулятор) и порт рядом */
            Nes *port = n;
            ref = make_nes_into(ref, rom, rom_size);
            ref->use_hooks = 0;
            xlat_register_all(port);
            port->use_hooks = 1;
            uint64_t r0 = ref->emu_steps, p0 = port->emu_steps, u0 = port->unwinds;
            int seg_bad = 0;
            for (int f = 0; f < seg_frames; f++) {
                nes_run_frame(port);
                /* порт останавливается на границе своей (возможно, переведённой) процедуры — это
                 * граница инструкции и для оригинала: доводим эталон до того же такта и сравниваем всё */
                while (ref->cpu.cycles < port->cpu.cycles && !ref->cpu.jammed) nes_step(ref);
                char why[256];
                if (ref->cpu.cycles != port->cpu.cycles) snprintf(why, sizeof why, "эталон не попал в такт порта");
                if (ref->cpu.cycles != port->cpu.cycles || diff_state(port, ref, why, sizeof why)) {
                    if (use_snaps) printf("снимок %d (кадр записи %u) + кадр %d: расхождение порт/оригинал — %s\n",
                                          si, cur_snap->frame, f, why);
                    else printf("кадр %d: расхождение порт/оригинал — %s\n", f, why);
                    seg_bad++;
                    break;     /* дальше этот отрезок сравнивать бессмысленно */
                }
            }
            bad += seg_bad;
            ref_steps += ref->emu_steps - r0; port_steps += port->emu_steps - p0; unw += port->unwinds - u0;
            if (bad >= 10) break;
        }
        total_frames += seg_frames;
        if (n->cpu.jammed) printf("  ! отрезок %d: процессор встал на недопустимой инструкции ($%04X)\n", si, n->cpu.pc);
    }

    if (is_cov) {
        FILE *o = fopen(argc > 5 ? argv[5] : "build/coverage.txt", "w");
        long cnt = 0;
        for (int b = 0; b < 8; b++)
            for (int a = 0x8000; a < 0x10000; a++)
                if (cov[b * 0x10000 + a]) { fprintf(o, "C %d %04X\n", b, a); cnt++; }
        for (int b = 0; b < 8; b++)
            for (int a = 0x8000; a < 0x10000; a++)
                if (rts_tgt[b * 0x10000 + a]) fprintf(o, "T %d %04X\n", b, a);
        fclose(o);
        printf("coverage: %ld instructions over %ld frames (%ld segments)\n", cnt, total_frames, segs);
        return 0;
    }
    if (is_check) {
        printf("check: %ld frames in %ld segments, %ld calls, %ld mismatches, %ld spanned a frame end, "
               "%ld not compared (no return in %d frames)\n", total_frames, segs, checks, check_fail, spans,
               check_skipped, FINISH_MAX_FRAMES);
        if (getenv("XT_VERBOSE"))
            for (int i = 0; i < xlat_count; i++)
                printf("  %-14s %8ld calls %6ld bad\n", xlat_table[i].name, per_fn_calls[i], per_fn_fail[i]);
        return check_fail ? 1 : 0;
    }
    if (is_prof) {
        uint32_t *c = (uint32_t *)cov;
        FILE *o = fopen(argc > 5 ? argv[5] : "build/profile.txt", "w");
        for (int b = 0; b < 8; b++)
            for (int a = 0x8000; a < 0x10000; a++)
                if (c[b * 0x10000 + a]) fprintf(o, "P %d %04X %u\n", b, a, c[b * 0x10000 + a]);
        fclose(o);
        printf("profile: emulated %llu instructions\n", (unsigned long long)n->emu_steps);
        return 0;
    }
    long tb = 0;
    for (int i = 0; i < xlat_count; i++) tb += xlat_table[i].bytes;
    (void)emu_bytes_exec; (void)trace_count;
    printf("game: %ld frames in %ld segments, %ld bad; translated routines %d (%ld bytes); "
           "instructions run by translated code: %.1f%%; unwinds %llu\n", total_frames, segs, bad, xlat_count, tb,
           100.0 * (1.0 - (double)port_steps / (double)(ref_steps ? ref_steps : 1)), (unsigned long long)unw);
    return bad ? 1 : 0;
}
