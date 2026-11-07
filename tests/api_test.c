/* api_test.c — проверка интерфейса tgl_api на Linux: тот же код, что уйдёт в tglcore.dll. */
#include <stdio.h>
#include <stdlib.h>
#include "../core/tgl_api.h"

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: api_test rom.nes [out.ppm]\n"); return 2; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) return 2;
    static uint8_t rom[1 << 20];
    int size = (int)fread(rom, 1, sizeof rom, f);
    fclose(f);
    int mode = argc > 3 ? atoi(argv[3]) : 1;
    printf("api version %d, load %d\n", tgl_api_version(), tgl_load_rom(rom, size));
    tgl_use_translation(mode);
    printf("translation %s, %d routines\n", mode ? "on" : "off", tgl_translated_routines());
    /* титульный экран → Start → Start → игра, держим «огонь» */
    for (int i = 1; i <= 900; i++) {
        int b = 0;
        if ((i >= 200 && i < 206) || (i >= 360 && i < 366)) b = 0x08;
        if (i > 450) b = 0x02;
        tgl_run_frame(b);
    }
    static uint8_t px[256 * 240 * 4];
    tgl_get_frame_rgba(px);
    long sum = 0;
    for (int i = 0; i < 256 * 240 * 4; i++) sum += px[i];
    printf("frames %d, picture checksum %ld, ram[$2A]=%d\n", tgl_frame_count(), sum, tgl_peek(0x2A));
    if (argc > 2) {
        FILE *o = fopen(argv[2], "wb");
        fprintf(o, "P6 256 240 255\n");
        for (int i = 0; i < 256 * 240; i++) fwrite(px + i * 4, 1, 3, o);
        fclose(o);
    }
    return 0;
}
