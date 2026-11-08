/* tgl_api.c — реализация интерфейса ядра (одна приставка на программу). */
#include "rt.h"
#include "nes.h"
#include "tgl_api.h"
#include "xlat.h"
#include "custom.h"

#define ROM_MAX (1 << 20)
static uint8_t rom_copy[ROM_MAX];
static Nes nes;
static int loaded;
static double audio_rate = 48000.0;   /* частота вывода звука */
/* Кнопки привязаны к номеру кадра: переданные в вызове k вступают в силу в такт конца
 * кадра k (одинаково с переводом и без него). Если переведённый код дошёл до конца кадра
 * раньше, чем пришли его кнопки, они применяются в начале следующего вызова — до этого
 * игра кнопки не читает (код сворачивается сразу за концом кадра, см. frame_done). */
static uint8_t buttons_of_call;
static int deferred;
static int cheats;                     /* бит 0 — бессмертие, бит 1 — убивать с одного попадания */

static void on_frame_end(Nes *n) {
    /* бессмертие: в конце каждого кадра жизнь (текущая и цель счётчика) снова равна максимуму */
    custom_cheats(n, cheats);
    uint64_t want = n->ppu.frames_done - 1;   /* номер вызова, чьи кнопки нужны */
    if (want <= n->frame) { n->pad_state = buttons_of_call; deferred = 0; }
    else deferred = 1;
}

int tgl_api_version(void) { return 4; }

int tgl_load_rom(const uint8_t *rom, int size) {
    if (!rom || size <= 0 || size > ROM_MAX) return -10;
    memcpy(rom_copy, rom, (size_t)size);
    int err = nes_load_rom(&nes, rom_copy, size);
    loaded = (err == 0);
    if (loaded) {
        nes.frame_end_fn = on_frame_end;
        xlat_register_all(&nes);    /* переведённые процедуры (если ядро собрано с ними) */
        custom_install(&nes);       /* новые враги модов */
        custom_mod_clear();         /* память модов — заново, моды кладутся после загрузки */
        nes.use_hooks = 1;
        nes.audio_rate = audio_rate;
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


void tgl_audio_set_rate(double hz) {
    if (hz < 8000 || hz > 200000) return;
    audio_rate = hz;
    nes.audio_rate = hz;
    apu_set_rate(&nes.apu, hz);
}

int tgl_audio_read(int16_t *out, int max) {
    if (!loaded || max <= 0) return 0;
    return apu_read(&nes.apu, out, max);
}

/* Правка данных ROM (моды): bank 0..7, addr — адрес процессора ($8000-$BFFF для банков 0-6,
 * $C000-$FFFF для банка 7). Пишет в копию ROM в памяти; файл на диске не меняется.
 * 0 = успех, <0 = адрес вне ROM. Переведённый код читает данные из этой копии, поэтому правки
 * данных действуют сразу; правки самого кода через перевод не действуют (см. tools/mod.py). */
int tgl_patch_rom(int bank, int addr, const uint8_t *data, int len) {
    if (!loaded || !data || len <= 0) return -1;
    if (bank == MOD_BANK) return custom_mod_write(addr, data, len);     /* память модов */
    int banks = nes.prg_banks;
    if (bank < 0 || bank >= banks) return -2;
    int base = (bank == banks - 1) ? 0xC000 : 0x8000;
    if (addr < base || addr + len > base + 0x4000) return -3;
    for (int i = 0; i < len; i++) nes.prg[bank * 0x4000 + (addr - base) + i] = data[i];
    return 0;
}

/* ---------------------------------------------------------------- сохранения (версия 4)
 * Снимок = заголовок + структура Nes целиком + кнопки API. Указатели (функции шины, ROM,
 * перехваты, обработчики) при загрузке остаются текущими: они не часть состояния игры.
 * Заголовок проверяет, что сборка ядра (размер состояния, число переведённых процедур)
 * и ROM вместе с модами те же, — иначе отказ, а не испорченная игра. */
typedef struct StateHeader {
    char magic[4];           /* "TGLS" */
    uint32_t version;        /* формат файла */
    uint32_t nes_size;       /* sizeof(Nes) этой сборки */
    uint32_t xlat;           /* число переведённых процедур этой сборки */
    uint32_t rom_sum;        /* контрольная сумма PRG (ROM с модами) */
    uint32_t extra;          /* кнопки: buttons_of_call | deferred << 8 */
} StateHeader;

static uint32_t prg_sum(void) {
    uint32_t h = 2166136261u;                      /* FNV-1a */
    int n = nes.prg_banks * 0x4000;
    for (int i = 0; i < n; i++) { h ^= nes.prg[i]; h *= 16777619u; }
    for (int i = 0; i < MOD_MEM_SIZE; i++) { h ^= custom_mod_mem[i]; h *= 16777619u; }
    return h;
}

int tgl_state_size(void) { return (int)(sizeof(StateHeader) + sizeof(Nes)); }

int tgl_state_save(uint8_t *buf, int cap) {
    if (!loaded || !buf || cap < tgl_state_size()) return -1;
    StateHeader h;
    memcpy(h.magic, "TGLS", 4);
    h.version = 1;
    h.nes_size = (uint32_t)sizeof(Nes);
    h.xlat = (uint32_t)xlat_count;
    h.rom_sum = prg_sum();
    h.extra = (uint32_t)buttons_of_call | ((uint32_t)deferred << 8);
    memcpy(buf, &h, sizeof h);
    memcpy(buf + sizeof h, &nes, sizeof nes);
    return tgl_state_size();
}

int tgl_state_load(const uint8_t *buf, int len) {
    if (!loaded || !buf || len != tgl_state_size()) return -1;
    StateHeader h;
    memcpy(&h, buf, sizeof h);
    if (h.magic[0] != 'T' || h.magic[1] != 'G' || h.magic[2] != 'L' || h.magic[3] != 'S' || h.version != 1) return -2;  /* не сохранение */
    if (h.nes_size != sizeof(Nes) || h.xlat != (uint32_t)xlat_count) return -3;  /* другая сборка ядра */
    if (h.rom_sum != prg_sum()) return -4;                               /* другой ROM или моды */
    /* сохранить то, что принадлежит этой программе, а не игре */
    Cpu keep_cpu = nes.cpu;
    uint8_t *prg = nes.prg;
    struct NesHooks *hooks = nes.hooks;
    void (*fe)(Nes *) = nes.frame_end_fn, (*tr)(Nes *) = nes.trace_fn, (*wf)(Nes *) = nes.watch_fn;
    void *user = nes.user;
    int use_hooks = nes.use_hooks;
    double rate = nes.audio_rate;
    memcpy(&nes, buf + sizeof h, sizeof nes);
    nes.cpu.read = keep_cpu.read; nes.cpu.write = keep_cpu.write; nes.cpu.ctx = keep_cpu.ctx;
    nes.prg = prg; nes.hooks = hooks;
    nes.frame_end_fn = fe; nes.trace_fn = tr; nes.watch_fn = wf; nes.user = user;
    nes.use_hooks = use_hooks;
    nes.hook_depth = 0; nes.xlat_bail = 0; nes.xlat_unwind = 0;
    if (nes.audio_rate != rate) { nes.audio_rate = rate; apu_set_rate(&nes.apu, rate); }
    nes.apu.ring_r = nes.apu.ring_w;               /* старый звук из буфера не проигрывать */
    buttons_of_call = (uint8_t)(h.extra & 0xFF);
    deferred = (int)((h.extra >> 8) & 1);
    return 0;
}

/* Описание сборки ядра: дата сборки, сколько процедур переведено, сколько новых врагов.
 * Показывается при запуске — сразу видно, какое ядро загружено. */
static int put(char *buf, int cap, int pos, const char *t) {
    while (*t && pos < cap - 1) buf[pos++] = *t++;
    buf[pos] = 0;
    return pos;
}
static int put_num(char *buf, int cap, int pos, int v) {
    char tmp[12]; int k = 0;
    if (v == 0) tmp[k++] = '0';
    while (v > 0 && k < 11) { tmp[k++] = (char)('0' + v % 10); v /= 10; }
    while (k > 0 && pos < cap - 1) buf[pos++] = tmp[--k];
    buf[pos] = 0;
    return pos;
}
int tgl_build_info(char *buf, int cap) {
    if (!buf || cap < 2) return 0;
    int p = 0;
    p = put(buf, cap, p, "ядро " __DATE__ " " __TIME__ ", перевод ");
    p = put_num(buf, cap, p, xlat_count);
    p = put(buf, cap, p, ", новых врагов ");
    p = put_num(buf, cap, p, custom_count);
    return p;
}

/* Читы: бит 0 — бессмертие, бит 1 — убивать с одного попадания (custom.c). Возвращает прежнее значение. */
int tgl_cheats(int flags) { int old = cheats; cheats = flags; return old; }
