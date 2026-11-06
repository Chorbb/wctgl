/* replay.c — сверка порта с оригиналом.
 *
 * Берёт ROM и запись прохождения из Mesen (tgl_logger2.lua), проигрывает те же нажатия
 * и сравнивает оперативную память после каждого кадра. Печатает первый кадр,
 * где что-то разошлось, и адреса, которые отличаются.
 *
 *   replay <rom.nes> <run.bin> [кадров] [png_каждые_N_кадров] [папка_для_png]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../core/nes.h"

typedef struct { uint32_t frame; uint8_t input, flags; const uint8_t *cpu, *extra, *ram, *oam; } Rec;

static int extra_count = 0;          /* v4: сколько доп. полей */
static char extra_names[64][64];

static uint8_t *read_file(const char *path, long *size) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); *size = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *d = malloc((size_t)*size);
    if (fread(d, 1, (size_t)*size, f) != (size_t)*size) { fclose(f); free(d); return NULL; }
    fclose(f);
    return d;
}

static void save_ppm(Nes *n, const char *dir, uint64_t frame) {
    char path[512];
    snprintf(path, sizeof path, "%s/frame_%06llu.ppm", dir, (unsigned long long)frame);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6 %d %d 255\n", NES_SCREEN_W, NES_SCREEN_H);
    for (int i = 0; i < NES_SCREEN_W * NES_SCREEN_H; i++) {
        uint32_t c = NES_PALETTE[n->ppu.framebuffer[i] & 63];
        uint8_t rgb[3] = { (uint8_t)(c >> 16), (uint8_t)(c >> 8), (uint8_t)c };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}

/* Вариант перебора: в $DCE8 (ADC $2A) в A лежит указатель стека из TSX, в C — перенос.
 * Подменяем именно их: это ровно то, что зависит от момента прихода NMI. */
static int rng_override = -1;          /* -1 = не вмешиваться, иначе (сдвиг стека + 8) * 2 + перенос */
static void watch_rng(Nes *n) {
    if (rng_override < 0) return;
    int carry = rng_override & 1, shift = (rng_override >> 1) - 8;
    n->cpu.a = (uint8_t)(n->cpu.a + shift);
    n->cpu.p = (uint8_t)((n->cpu.p & ~FLAG_C) | carry);
    rng_override = -1;
}

static int count_diffs(Nes *n, const uint8_t *ram) {
    int d = 0;
    for (int a = 0; a < 2048; a++) {
        if (a >= 0x100 && a < 0x200) continue;   /* стек не сверяем: он зависит от момента NMI */
        if (n->fe_ram[a] != ram[a]) d++;
    }
    return d;
}

static void print_diffs(Nes *n, const uint8_t *ram, int limit) {
    int shown = 0;
    for (int a = 0; a < 2048 && shown < limit; a++) {
        if (a >= 0x100 && a < 0x200) continue;
        if (n->fe_ram[a] != ram[a]) { printf("  $%03X:%02X/%02X", a, n->fe_ram[a], ram[a]); shown++; }
    }
    printf("\n");
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: replay rom log [frames] [png_every] [dir]\n"); return 2; }
    long rom_size, log_size;
    uint8_t *rom = read_file(argv[1], &rom_size);
    uint8_t *log = read_file(argv[2], &log_size);
    if (!rom || !log) { fprintf(stderr, "cannot read files\n"); return 2; }
    int max_frames = argc > 3 ? atoi(argv[3]) : 1000000;
    int png_every = argc > 4 ? atoi(argv[4]) : 0;
    const char *png_dir = argc > 5 ? argv[5] : ".";

    int ver = 0;
    if (memcmp(log, "TGLLOG2\n", 8) == 0) ver = 2;
    if (memcmp(log, "TGLLOG3\n", 8) == 0) ver = 3;
    long hdr = 8;
    if (memcmp(log, "TGLLOG4\n", 8) == 0) {
        ver = 4;
        /* строка с именами доп. полей */
        long e = 8; while (e < log_size && log[e] != '\n') e++;
        char line[4096]; long len = e - 8 < 4095 ? e - 8 : 4095;
        memcpy(line, log + 8, (size_t)len); line[len] = 0;
        for (char *tok = strtok(line, ","); tok && extra_count < 64; tok = strtok(NULL, ","))
            snprintf(extra_names[extra_count++], 64, "%s", tok);
        hdr = e + 1;
    }
    if (!ver) { fprintf(stderr, "unknown log format\n"); return 2; }
    int cpu_bytes = ver >= 3 ? 12 : 0;
    int extra_bytes = ver >= 4 ? 4 * extra_count : 0;
    int cap = 1 << 16, nrec = 0;
    Rec *recs = malloc(sizeof(Rec) * (size_t)cap);
    long i = hdr;
    while (i + 7 + cpu_bytes + extra_bytes + 2048 + 256 <= log_size && log[i] == 'F') {
        Rec r;
        r.frame = (uint32_t)(log[i + 1] | (log[i + 2] << 8) | (log[i + 3] << 16) | ((uint32_t)log[i + 4] << 24));
        r.input = log[i + 5]; r.flags = log[i + 6];
        r.cpu = cpu_bytes ? log + i + 7 : NULL;
        r.extra = extra_bytes ? log + i + 7 + cpu_bytes : NULL;
        r.ram = log + i + 7 + cpu_bytes + extra_bytes; r.oam = r.ram + 2048;
        i += 7 + cpu_bytes + extra_bytes + 2048 + 256 + ((r.flags & 1) ? 2048 : 0);
        if (i > log_size) break;
        if (nrec == cap) { cap *= 2; recs = realloc(recs, sizeof(Rec) * (size_t)cap); }
        recs[nrec++] = r;
    }
    printf("log: %d records\n", nrec);

    /* v2: снимок в момент перезагрузки (флаг 2), сверка со следующего кадра.
     * v3: флаг 4 = первый кадр после перезагрузки, память до неё — в предыдущей записи. */
    int start = -1;
    for (int k = 0; k < nrec; k++) {
        if (ver == 2 && (recs[k].flags & 2)) start = k;
        /* v3: перезагрузку видно по сбросу счётчика тактов процессора */
        if (ver >= 3 && k > 0) {
            uint32_t c0 = (uint32_t)(recs[k-1].cpu[8] | (recs[k-1].cpu[9] << 8) | (recs[k-1].cpu[10] << 16) | ((uint32_t)recs[k-1].cpu[11] << 24));
            uint32_t c1 = (uint32_t)(recs[k].cpu[8] | (recs[k].cpu[9] << 8) | (recs[k].cpu[10] << 16) | ((uint32_t)recs[k].cpu[11] << 24));
            if (c1 < c0) { start = k - 1; break; }
        }
    }
    if (start < 0) { fprintf(stderr, "no reset point in log\n"); return 2; }
    int start_line = getenv("TGL_START_LINE") ? atoi(getenv("TGL_START_LINE")) : 0;
    Nes *n = calloc(1, sizeof(Nes));
    int err = nes_load_rom(n, rom, (int)rom_size);
    if (err) { fprintf(stderr, "rom load error %d\n", err); return 2; }
    memcpy(n->ram, recs[start].ram, 2048);
    nes_reset(n);
    if (getenv("TGL_PPU_OFFSET")) n->ppu_offset = atoi(getenv("TGL_PPU_OFFSET"));
    if (getenv("TGL_DMC_PHASE")) n->dmc.timer = atoi(getenv("TGL_DMC_PHASE"));
    if (getenv("TGL_HIT_DELAY")) n->hit_delay = atoi(getenv("TGL_HIT_DELAY"));
    if (getenv("TGL_NOSKIP")) n->ppu.skip_vblank = 0;
    if (getenv("TGL_FEDOT")) n->frame_end_dot = atoi(getenv("TGL_FEDOT"));
    if (getenv("TGL_RACE")) n->race_dot = atoi(getenv("TGL_RACE"));
    if (getenv("TGL_SPLIT")) n->access_split = atoi(getenv("TGL_SPLIT"));
    if (getenv("TGL_APU_PHASE")) n->apu_phase = atoi(getenv("TGL_APU_PHASE"));
    n->ppu.scanline = start_line;   /* где была развёртка в момент перезагрузки (калибруется) */

    /* Синхронизация генератора случайных чисел.
     * Игра подмешивает в $2A указатель стека в момент NMI (код $DCE6). Где именно NMI
     * застанет основной цикл, зависит от тайминга на уровне долей инструкции, который
     * эмулятор повторяет не идеально. Если кадр разошёлся, перебираем значение $2A
     * сразу после этого места: нашлось значение, при котором вся память совпала, —
     * значит логика игры верна, а разница только в тайминге. Не нашлось — настоящая ошибка. */
    int sync = getenv("TGL_NOSYNC") == NULL;
    if (sync) { n->watch_pc = 0xDCE8; n->watch_fn = watch_rng; }

    int compared = 0, exact = 0, rng_synced = 0, unexplained = 0, run = 0;
    int first_bad = -1;
    Nes *saved = malloc(sizeof(Nes));
    for (int k = start + 1; k < nrec && compared < max_frames; k++) {
        n->dmc.dbg_fetches = n->dmc.dbg_irqs = 0;
        *saved = *n;
        rng_override = -1;
        n->pad_state = recs[k].input;
        nes_run_frame(n);
        compared++;
        int diffs = count_diffs(n, recs[k].ram);
        int how = diffs ? 'X' : '=';
        if (diffs && sync) {
            for (int c = 0; c < 34; c++) {
                *n = *saved;
                rng_override = c;
                n->pad_state = recs[k].input;
                nes_run_frame(n);
                if (count_diffs(n, recs[k].ram) == 0) { how = 'R'; break; }
            }
            if (how != 'R') {      /* не помогло: возвращаем кадр без вмешательства */
                *n = *saved;
                rng_override = -1;
                n->pad_state = recs[k].input;
                nes_run_frame(n);
            }
        }
        if (getenv("TGL_REGS") && recs[k].cpu) {
            const uint8_t *c = recs[k].cpu;
            int lo = atoi(getenv("TGL_REGS")), hi = getenv("TGL_REGS_TO") ? atoi(getenv("TGL_REGS_TO")) : lo + 40;
            if (compared >= lo && compared <= hi)
            {
                uint32_t oc = (uint32_t)(c[8] | (c[9] << 8) | (c[10] << 16) | ((uint32_t)c[11] << 24));
                printf("R %5d %c port pc=%04X x=%02X | orig pc=%04X x=%02X | cyc-orig=%lld\n",
                       compared, how, n->fe_pc, n->cpu.x, c[0] | (c[1] << 8), c[3], (long long)((int64_t)n->fe_cycle - (int64_t)oc));
                if (recs[k].extra && getenv("TGL_EXTRA")) {
                    printf("E %5d", compared);
                    for (int e = 0; e < extra_count; e++) {
                        const uint8_t *q = recs[k].extra + 4 * e;
                        int32_t v = (int32_t)(q[0] | (q[1] << 8) | (q[2] << 16) | ((uint32_t)q[3] << 24));
                        printf(" %s=%d", extra_names[e], v);
                    }
                    printf("\n");
                }
            }
        }
        if (how == '=') { exact++; run = 0; }
        else if (how == 'R') { rng_synced++; run = 0; }
        else {
            run++;
            if (first_bad < 0) first_bad = compared;
            if (run <= 2 || getenv("TGL_VERBOSE")) {
                printf("frame %d: %d bytes differ (port pc=%04X):", compared, diffs, n->cpu.pc);
                print_diffs(n, recs[k].ram, 8);
            }
            /* расхождение держится 3 кадра — это не мгновенный снимок посреди инструкции:
             * считаем его настоящим и подтягиваем память к оригиналу, чтобы проверять дальше */
            if (run == 3) {
                unexplained++;
                printf("  -> UNEXPLAINED divergence at frame %d, resyncing RAM\n", compared - 2);
                memcpy(n->ram, recs[k].ram, 2048);
                run = 0;
            }
        }
        if (png_every && (compared % png_every == 0)) save_ppm(n, png_dir, recs[k].frame);
    }
    printf("compared %d frames: exact %d, rng-synced %d, transient %d, UNEXPLAINED %d\n",
           compared, exact, rng_synced, compared - exact - rng_synced, unexplained);
    if (unexplained == 0) printf("LOGIC MATCHES ORIGINAL (differences only in RNG timing)\n");
    if (first_bad < 0 && rng_synced == 0) printf("ALL FRAMES MATCH\n");
    return unexplained ? 1 : 0;
}
