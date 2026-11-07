/* snaplog.h — запись tgl_logger5 (Mesen): кнопки каждого кадра и снимки приставки.
 *
 * Снимок сделан на входе в обработчик NMI ($DC75) — на границе инструкции, поэтому
 * наш эмулятор может продолжить ровно оттуда. Тайминг внутри кадра восстанавливается
 * приблизительно (строка/точка видеочипа, счётчик DMC): эталон для проверки перевода —
 * наш эмулятор, поэтому важно лишь, чтобы игра из снимка шла правдоподобно.
 */
#ifndef TGL_SNAPLOG_H
#define TGL_SNAPLOG_H
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../core/nes.h"

typedef struct Snap {
    uint32_t frame;          /* номер кадра записи, после конца которого сделан снимок */
    const char *text;        /* состояние Mesen: строки «ключ=значение» */
    int text_len;
    const uint8_t *ram, *oam, *nt, *chr, *pal;
} Snap;

typedef struct SnapLog {
    uint8_t *data;
    long size;
    int nsnaps;
    Snap *snaps;
    uint32_t frames;         /* сколько кадров записано */
    uint8_t *input;          /* кнопки кадра f (f = 1..frames) */
} SnapLog;

static uint32_t sl_u32(const uint8_t *p) { return (uint32_t)(p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24)); }

static int snaplog_load(SnapLog *L, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END); L->size = ftell(f); fseek(f, 0, SEEK_SET);
    L->data = malloc((size_t)L->size);
    if (fread(L->data, 1, (size_t)L->size, f) != (size_t)L->size) { fclose(f); return -1; }
    fclose(f);
    if (L->size < 9 || memcmp(L->data, "TGLSNAP1\n", 9)) return -2;
    /* первый проход — сосчитать */
    long i = 9; int ns = 0; uint32_t maxf = 0;
    while (i < L->size) {
        uint8_t t = L->data[i];
        if (t == 'I') { uint32_t fr = sl_u32(L->data + i + 1); if (fr > maxf) maxf = fr; i += 6; }
        else if (t == 'S') { uint32_t ln = sl_u32(L->data + i + 6); i += 10 + ln + 2048 + 256 + 2048 + 8192 + 32; ns++; }
        else return -3;
    }
    if (i != L->size) return -4;
    L->frames = maxf;
    L->input = calloc(maxf + 2, 1);
    L->snaps = calloc((size_t)ns, sizeof(Snap));
    L->nsnaps = ns;
    i = 9; ns = 0;
    while (i < L->size) {
        uint8_t t = L->data[i];
        if (t == 'I') { L->input[sl_u32(L->data + i + 1)] = L->data[i + 5]; i += 6; continue; }
        Snap *s = &L->snaps[ns++];
        s->frame = sl_u32(L->data + i + 1);
        uint32_t ln = sl_u32(L->data + i + 6);
        s->text = (const char *)L->data + i + 10;
        s->text_len = (int)ln;
        const uint8_t *p = L->data + i + 10 + ln;
        s->ram = p; s->oam = p + 2048; s->nt = p + 2304; s->chr = p + 4352; s->pal = p + 12544;
        i += 10 + ln + 2048 + 256 + 2048 + 8192 + 32;
    }
    return 0;
}

/* значение по ключу из текста снимка; true/false → 1/0; нет ключа → def */
static long snap_get(const Snap *s, const char *key, long def) {
    size_t kl = strlen(key);
    const char *p = s->text, *end = s->text + s->text_len;
    while (p < end) {
        const char *e = memchr(p, '\n', (size_t)(end - p));
        if (!e) e = end;
        if ((size_t)(e - p) > kl && !memcmp(p, key, kl) && p[kl] == '=') {
            const char *v = p + kl + 1;
            if (!strncmp(v, "true", 4)) return 1;
            if (!strncmp(v, "false", 5)) return 0;
            return strtol(v, NULL, 10);
        }
        p = e + 1;
    }
    return def;
}

/* Состояние приставки из снимка. ROM уже загружен в n (nes_load_rom). */
static void snap_apply(Nes *n, const Snap *s) {
    nes_reset(n);
    memcpy(n->ram, s->ram, 2048);
    memcpy(n->ppu.oam, s->oam, 256);
    memcpy(n->ppu.nt, s->nt, 2048);
    memcpy(n->chr, s->chr, 8192);
    memcpy(n->ppu.pal, s->pal, 32);
    /* банк: игра держит текущий банк в $04 (так надёжнее, чем следить за записями) */
    n->bank = n->ram[0x04] & 7;

    Cpu *c = &n->cpu;
    c->a = (uint8_t)snap_get(s, "cpu.a", 0);
    c->x = (uint8_t)snap_get(s, "cpu.x", 0);
    c->y = (uint8_t)snap_get(s, "cpu.y", 0);
    c->s = (uint8_t)snap_get(s, "cpu.sp", 0xFD);
    c->p = (uint8_t)(snap_get(s, "cpu.ps", 0x24) | 0x20);
    c->pc = (uint16_t)snap_get(s, "cpu.pc", 0xDC75);

    Ppu *p = &n->ppu;
    long t = snap_get(s, "ppu.tmpVideoRamAddr", 0);
    p->ctrl = (uint8_t)((t >> 10) & 3);
    if (snap_get(s, "ppu.control.verticalWrite", 0)) p->ctrl |= 0x04;
    if (snap_get(s, "ppu.control.spritePatternAddr", 0)) p->ctrl |= 0x08;
    if (snap_get(s, "ppu.control.backgroundPatternAddr", 0)) p->ctrl |= 0x10;
    if (snap_get(s, "ppu.control.largeSprites", 0)) p->ctrl |= 0x20;
    if (snap_get(s, "ppu.control.nmiOnVerticalBlank", 0)) p->ctrl |= 0x80;
    p->mask = 0;
    if (snap_get(s, "ppu.mask.grayscale", 0)) p->mask |= 0x01;
    if (snap_get(s, "ppu.mask.backgroundMask", 0)) p->mask |= 0x02;
    if (snap_get(s, "ppu.mask.spriteMask", 0)) p->mask |= 0x04;
    if (snap_get(s, "ppu.mask.backgroundEnabled", 0)) p->mask |= 0x08;
    if (snap_get(s, "ppu.mask.spritesEnabled", 0)) p->mask |= 0x10;
    if (snap_get(s, "ppu.mask.intensifyRed", 0)) p->mask |= 0x20;
    if (snap_get(s, "ppu.mask.intensifyGreen", 0)) p->mask |= 0x40;
    if (snap_get(s, "ppu.mask.intensifyBlue", 0)) p->mask |= 0x80;
    p->status = 0;
    if (snap_get(s, "ppu.statusFlags.verticalBlank", 0)) p->status |= 0x80;
    if (snap_get(s, "ppu.statusFlags.sprite0Hit", 0)) p->status |= 0x40;
    if (snap_get(s, "ppu.statusFlags.spriteOverflow", 0)) p->status |= 0x20;
    p->v = (uint16_t)snap_get(s, "ppu.videoRamAddr", 0);
    p->t = (uint16_t)t;
    p->x = (uint8_t)snap_get(s, "ppu.xScroll", 0);
    p->w = (uint8_t)snap_get(s, "ppu.writeToggle", 0);
    p->read_buffer = (uint8_t)snap_get(s, "ppu.memoryReadBuffer", 0);
    p->oam_addr = (uint8_t)snap_get(s, "ppu.spriteRamAddr", 0);
    p->scanline = (int)snap_get(s, "ppu.scanline", 241);
    p->dot = (int)snap_get(s, "ppu.cycle", 0);
    p->frame_odd = (int)(snap_get(s, "ppu.frameCount", 0) & 1);
    p->time = (uint64_t)(((int64_t)n->master - n->ppu_offset) / 4);

    Dmc *d = &n->dmc;
    d->irq_enabled = (int)snap_get(s, "apu.dmc.irqEnabled", 0);
    d->loop = (int)snap_get(s, "apu.dmc.loopFlag", 0);
    d->period = (int)snap_get(s, "apu.dmc.timer.period", 427) + 1;
    d->timer = (int)snap_get(s, "apu.dmc.timer.timer", 0) + 1;
    d->sample_addr = (uint16_t)snap_get(s, "apu.dmc.sampleAddr", 0xC000);
    d->sample_len = (int)snap_get(s, "apu.dmc.sampleLength", 1);
    d->cur_addr = (uint16_t)snap_get(s, "apu.dmc.currentAddr", 0xC000);
    d->bytes_left = (int)snap_get(s, "apu.dmc.bytesRemaining", 0);
    d->bits_left = (int)snap_get(s, "apu.dmc.bitsRemaining", 0);
    d->buffer_full = !snap_get(s, "apu.dmc.bufferEmpty", 1);
    d->buffer = (uint8_t)snap_get(s, "apu.dmc.readBuffer", 0);
    d->shift = (uint8_t)snap_get(s, "apu.dmc.shiftRegister", 0);
    d->silence = (int)snap_get(s, "apu.dmc.silenceFlag", 1);
    d->irq_flag = 0;

    /* NMI уже принят (мы на входе в обработчик): линия поднята, нового фронта нет */
    c->nmi_level = (p->status & 0x80) && (p->ctrl & 0x80);
    c->prev_nmi_level = c->nmi_level;
    c->need_nmi = c->prev_need_nmi = 0;
    c->irq_line = 0;
    c->run_irq = c->prev_run_irq = 0;
}
#endif
