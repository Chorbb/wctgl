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
