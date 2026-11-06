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

typedef struct { uint32_t frame; uint8_t input, flags; const uint8_t *cpu, *ram, *oam; } Rec;

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
    if (!ver) { fprintf(stderr, "unknown log format\n"); return 2; }
    int cpu_bytes = ver == 3 ? 12 : 0;
    int cap = 1 << 16, nrec = 0;
    Rec *recs = malloc(sizeof(Rec) * (size_t)cap);
    long i = 8;
    while (i + 7 + cpu_bytes + 2048 + 256 <= log_size && log[i] == 'F') {
        Rec r;
        r.frame = (uint32_t)(log[i + 1] | (log[i + 2] << 8) | (log[i + 3] << 16) | ((uint32_t)log[i + 4] << 24));
        r.input = log[i + 5]; r.flags = log[i + 6];
        r.cpu = cpu_bytes ? log + i + 7 : NULL;
        r.ram = log + i + 7 + cpu_bytes; r.oam = r.ram + 2048;
        i += 7 + cpu_bytes + 2048 + 256 + ((r.flags & 1) ? 2048 : 0);
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
        if (ver == 3 && k > 0) {
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
    n->dot_offset = getenv("TGL_DOT_OFFSET") ? atoi(getenv("TGL_DOT_OFFSET")) : 0;
    n->nmi_delay = getenv("TGL_NMI_DELAY") ? atoi(getenv("TGL_NMI_DELAY")) : 0;
    if (getenv("TGL_DMC_PHASE")) n->dmc.timer = atoi(getenv("TGL_DMC_PHASE"));
    if (getenv("TGL_DMC_STALL")) n->dmc.stall_cost = atoi(getenv("TGL_DMC_STALL"));
    n->ppu.scanline = start_line;   /* где была развёртка в момент перезагрузки (калибруется) */

    int first_bad = -1, bad_frames = 0, compared = 0, run = 0, persistent = -1;
    for (int k = start + 1; k < nrec && compared < max_frames; k++) {
        n->pad_state = recs[k].input;
        nes_run_frame(n);
        compared++;
        int diffs = 0;
        for (int a = 0; a < 2048; a++) {
            if (a >= 0x100 && a < 0x200) continue;   /* стек сверяем отдельно: он зависит от момента NMI */
            if (n->ram[a] != recs[k].ram[a]) diffs++;
        }
        if (png_every && (compared % png_every == 0)) save_ppm(n, png_dir, recs[k].frame);
        if (getenv("TGL_TRACE") && compared <= atoi(getenv("TGL_TRACE")))
        {
            printf("  frame %u: %d bytes differ, port pc=%04X orig pc=%04X", recs[k].frame, diffs, n->cpu.pc,
                   recs[k].cpu ? (recs[k].cpu[0] | (recs[k].cpu[1] << 8)) : 0);
            int sh = 0;
            for (int a = 0; a < 2048 && sh < 6; a++) {
                if (a >= 0x100 && a < 0x200) continue;
                if (n->ram[a] != recs[k].ram[a]) { printf("  $%03X:%02X/%02X", a, n->ram[a], recs[k].ram[a]); sh++; }
            }
            printf("\n");
        }
        run = diffs ? run + 1 : 0;
        if (run == 5 && persistent < 0) persistent = compared - 4;
        if (diffs) {
            bad_frames++;
            if (first_bad < 0) {
                first_bad = (int)recs[k].frame;
                printf("FIRST MISMATCH at frame %u (%d after reset, %d bytes differ). port pc=%04X",
                       recs[k].frame, compared, diffs, n->cpu.pc);
                if (recs[k].cpu) printf(" original pc=%04X", recs[k].cpu[0] | (recs[k].cpu[1] << 8));
                printf(" bank=%d\n", n->bank);
                int shown = 0;
                for (int a = 0; a < 2048 && shown < 24; a++) {
                    if (a >= 0x100 && a < 0x200) continue;
                    if (n->ram[a] != recs[k].ram[a]) {
                        printf("  $%04X: port %02X  original %02X\n", a, n->ram[a], recs[k].ram[a]);
                        shown++;
                    }
                }
                if (png_every) save_ppm(n, png_dir, recs[k].frame);
            }
        }
    }
    printf("compared %d frames, mismatched %d, cpu jammed=%d, hooks=%llu\n",
           compared, bad_frames, n->cpu.jammed, (unsigned long long)n->hook_calls);
    if (persistent >= 0) printf("PERSISTENT DIVERGENCE from frame %d after reset\n", persistent);
    if (first_bad < 0) printf("ALL FRAMES MATCH\n");
    return first_bad < 0 ? 0 : 1;
}
