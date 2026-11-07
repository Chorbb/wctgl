/* state_test.c — сохранение и загрузка: после загрузки игра идёт точно так же, как шла бы.
 *   state_test rom.nes
 * Играем до кадра 600, сохраняем, играем ещё 600 → картинка/память A. Загружаем, повторяем → B.
 * A и B должны совпасть до байта: с переводом, без перевода и при загрузке «с переводом → без». */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../core/tgl_api.h"

static int buttons(int i) {               /* те же нажатия, что в api_test: Start, потом движение и стрельба */
    if (i >= 200 && i < 206) return 0x08;
    if (i >= 360 && i < 366) return 0x08;
    if (i > 500) return ((i / 30) & 1 ? 0x80 : 0x40) | ((i / 7) & 1 ? 0x02 : 0) | ((i / 90) & 1 ? 0x10 : 0x20);
    return 0;
}

static unsigned long run(int from, int to, uint8_t *ram) {
    static uint8_t px[256 * 240 * 4];
    for (int i = from; i < to; i++) tgl_run_frame(buttons(i));
    tgl_get_frame_rgba(px);
    unsigned long s = 0;
    for (int i = 0; i < (int)sizeof px; i++) s = s * 31 + px[i];
    for (int a = 0; a < 2048; a++) ram[a] = (uint8_t)tgl_peek(a);
    return s;
}

int main(int argc, char **argv) {
    if (argc < 2) return 2;
    FILE *f = fopen(argv[1], "rb");
    if (!f) return 2;
    static uint8_t rom[1 << 20];
    int size = (int)fread(rom, 1, sizeof rom, f);
    fclose(f);
    int bad = 0;
    for (int mode = 0; mode < 3; mode++) {
        tgl_load_rom(rom, size);
        tgl_use_translation(mode != 1);
        uint8_t ra[2048], rb[2048];
        run(0, 600, ra);
        int n = tgl_state_size();
        uint8_t *st = malloc((size_t)n);
        if (tgl_state_save(st, n) != n) { printf("save failed\n"); return 1; }
        unsigned long a = run(600, 1200, ra);
        if (mode == 2) tgl_use_translation(0);   /* сохранено с переводом, грузим без */
        int e = tgl_state_load(st, n);
        unsigned long b = run(600, 1200, rb);
        int same = e == 0 && a == b && !memcmp(ra, rb, sizeof ra);
        printf("%s: load=%d  %s\n", mode == 0 ? "перевод" : mode == 1 ? "эмулятор" : "перевод→эмулятор", e,
               same ? "совпало" : "РАСХОЖДЕНИЕ");
        bad += !same;
        free(st);
    }
    /* чужое сохранение: другой ROM (правка байта) — должно отказаться */
    tgl_load_rom(rom, size);
    int n = tgl_state_size(); uint8_t *st = malloc((size_t)n);
    tgl_state_save(st, n);
    uint8_t x = 0x42; tgl_patch_rom(7, 0xD0C7, &x, 1);
    int e = tgl_state_load(st, n);
    printf("другой ROM: load=%d (%s)\n", e, e == -4 ? "отказ, как надо" : "ОШИБКА");
    bad += e != -4;
    st[0] = 'X';
    e = tgl_state_load(st, n);
    printf("не сохранение: load=%d (%s)\n", e, e == -2 ? "отказ, как надо" : "ОШИБКА");
    bad += e != -2;
    printf("state size %d bytes\n", n);
    return bad ? 1 : 0;
}
