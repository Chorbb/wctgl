/* tgl_api.c — реализация интерфейса ядра (одна приставка на программу). */
#include "rt.h"
#include "nes.h"
#include "tgl_api.h"
#include "xlat.h"

#define ROM_MAX (1 << 20)
static uint8_t rom_copy[ROM_MAX];
static Nes nes;
static int loaded;
/* Кнопки привязаны к номеру кадра: переданные в вызове k вступают в силу в такт конца
 * кадра k (одинаково с переводом и без него). Если переведённый код дошёл до конца кадра
 * раньше, чем пришли его кнопки, они применяются в начале следующего вызова — до этого
 * игра кнопки не читает (код сворачивается сразу за концом кадра, см. frame_done). */
static uint8_t buttons_of_call;
static int deferred;
static void on_frame_end(Nes *n) {
    uint64_t want = n->ppu.frames_done - 1;   /* номер вызова, чьи кнопки нужны */
    if (want <= n->frame) { n->pad_state = buttons_of_call; deferred = 0; }
    else deferred = 1;
}

int tgl_api_version(void) { return 1; }

int tgl_load_rom(const uint8_t *rom, int size) {
    if (!rom || size <= 0 || size > ROM_MAX) return -10;
    memcpy(rom_copy, rom, (size_t)size);
    int err = nes_load_rom(&nes, rom_copy, size);
    loaded = (err == 0);
    if (loaded) {
        nes.frame_end_fn = on_frame_end;
        xlat_register_all(&nes);    /* переведённые процедуры (если ядро собрано с ними) */
        nes.use_hooks = 1;
        nes_reset(&nes);
    }
    return err;
}

void tgl_reset(void) {
    if (loaded) nes_reset(&nes);
}

void tgl_run_frame(int buttons) {
    if (!loaded) return;
    buttons_of_call = (uint8_t)buttons;
    if (deferred) { nes.pad_state = buttons_of_call; deferred = 0; }
    nes_run_frame(&nes);
}

void tgl_get_frame_rgba(uint8_t *out) {
    if (!out) return;
    for (int i = 0; i < NES_SCREEN_W * NES_SCREEN_H; i++) {
        uint32_t c = NES_PALETTE[nes.shown[i] & 63];
        out[i * 4 + 0] = (uint8_t)(c >> 16);
        out[i * 4 + 1] = (uint8_t)(c >> 8);
        out[i * 4 + 2] = (uint8_t)c;
        out[i * 4 + 3] = 255;
    }
}

int tgl_frame_count(void) { return (int)nes.frame; }

int tgl_peek(int addr) {
    if (!loaded) return -1;
    return nes.fe_ram[addr & 0x7FF];   /* память в такт конца последнего кадра */
}

void tgl_use_translation(int on) { nes.use_hooks = on ? 1 : 0; }

int tgl_translated_routines(void) { return xlat_count; }
