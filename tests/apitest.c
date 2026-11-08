/* apitest.c — игра через интерфейс ядра (tgl_api.c, как в Godot): от снимка записи, кнопки
 * записи, читы. Ловит то, что видно только через API (вызовы кадров, читы, сохранения).
 *
 *   build/apitest rom.nes snap.bin снимок кадров читы
 */
#include <stdio.h>
#include <stdlib.h>
#include "../core/tgl_api.c"
#include "reclog.h"
#include "snaplog.h"

static SnapLog sl;

int main(int argc, char **argv) {
    if (argc < 6) { fprintf(stderr, "usage: apitest rom.nes snap.bin snap frames cheats\n"); return 2; }
    long size; uint8_t *rom = rl_read_file(argv[1], &size);
    if (!rom || snaplog_load(&sl, argv[2])) { fprintf(stderr, "cannot read\n"); return 2; }
    int si = atoi(argv[3]), frames = atoi(argv[4]), ch = atoi(argv[5]);
    if (tgl_load_rom(rom, (int)size)) { fprintf(stderr, "load error\n"); return 2; }
    const Snap *s = &sl.snaps[si];
    snap_apply(&nes, s);
    nes.frame = nes.ppu.frames_done;
    nes.hook_depth = 0; nes.xlat_bail = 0; nes.xlat_unwind = 0;
    static uint8_t rgba[256 * 240 * 4];
    for (int f = 0; f < frames; f++) {
        if (f == 60) tgl_cheats(ch);                       /* как нажатие F8/F9 во время игры */
        uint64_t k = s->frame + 1 + (uint64_t)f;
        tgl_run_frame(k < sl.frames + 1 ? sl.input[k] : 0);
        tgl_get_frame_rgba(rgba);
        if (f % 300 == 0) printf("кадр %d: комната %d,%d жизнь %d\n", f, tgl_peek(0x52), tgl_peek(0x53), tgl_peek(0x48));
    }
    printf("apitest: %d кадров, ок\n", frames);
    return 0;
}
