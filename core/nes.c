/* nes.c — шина, картридж UNROM, PPU (по строкам), геймпад. */
#include <string.h>
#include "nes.h"

/* Стандартная палитра 2C02 (приближение к реальному выводу NTSC). */
const uint32_t NES_PALETTE[64] = {
    0x666666,0x002A88,0x1412A7,0x3B00A4,0x5C007E,0x6E0040,0x6C0600,0x561D00,
    0x333500,0x0B4800,0x005200,0x004F08,0x00404D,0x000000,0x000000,0x000000,
    0xADADAD,0x155FD9,0x4240FF,0x7527FE,0xA01ACC,0xB71E7B,0xB53120,0x994E00,
    0x6B6D00,0x388700,0x0C9300,0x008F32,0x007C8D,0x000000,0x000000,0x000000,
    0xFFFEFF,0x64B0FF,0x9290FF,0xC676FF,0xF36AFF,0xFE6ECC,0xFE8170,0xEA9E22,
    0xBCBE00,0x88D800,0x5CE430,0x45E082,0x48CDDE,0x4F4F4F,0x000000,0x000000,
    0xFFFEFF,0xC0DFFF,0xD3D2FF,0xE8C8FF,0xFBC2FF,0xFEC4EA,0xFECCC5,0xF7D8A5,
    0xE4E594,0xCFEF96,0xBDF4AB,0xB3F3CC,0xB5EBF2,0xB8B8B8,0x000000,0x000000,
};

static void ppu_advance_to(Nes *n, uint64_t target);
static uint64_t cpu_now(Nes *n) {
    int64_t t = (int64_t)(n->cpu.cycles + (uint64_t)n->cpu.access_offset) * 3 + n->dot_offset;
    return t < 0 ? 0 : (uint64_t)t;
}

/* ---------------------------------------------------------------- память PPU */

static uint8_t ppu_mem_read(Nes *n, uint16_t a) {
    a &= 0x3FFF;
    if (a < 0x2000) return n->chr[a];
    if (a < 0x3F00) return n->ppu.nt[a & 0x07FF];   /* вертикальное зеркалирование */
    uint8_t i = a & 0x1F;
    if ((i & 0x13) == 0x10) i &= 0x0F;
    return n->ppu.pal[i];
}

static void ppu_mem_write(Nes *n, uint16_t a, uint8_t v) {
    a &= 0x3FFF;
    if (a < 0x2000) { n->chr[a] = v; return; }
    if (a < 0x3F00) { n->ppu.nt[a & 0x07FF] = v; return; }
    uint8_t i = a & 0x1F;
    if ((i & 0x13) == 0x10) i &= 0x0F;
    n->ppu.pal[i] = v & 0x3F;
}

/* ---------------------------------------------------------------- регистры PPU */

static uint8_t ppu_reg_read(Nes *n, int r) {
    Ppu *p = &n->ppu;
    switch (r) {
    case 2: {
        uint8_t v = (uint8_t)((p->status & 0xE0) | (p->io_latch & 0x1F));
        p->status &= 0x7F;
        p->w = 0;
        p->io_latch = v;
        return v;
    }
    case 4: p->io_latch = p->oam[p->oam_addr]; return p->io_latch;
    case 7: {
        uint16_t a = p->v & 0x3FFF;
        uint8_t v;
        if (a < 0x3F00) { v = p->read_buffer; p->read_buffer = ppu_mem_read(n, a); }
        else { v = ppu_mem_read(n, a); p->read_buffer = ppu_mem_read(n, a - 0x1000); }
        p->v = (uint16_t)(p->v + ((p->ctrl & 4) ? 32 : 1));
        p->io_latch = v;
        return v;
    }
    }
    return p->io_latch;   /* регистры только для записи возвращают содержимое шины */
}

static void ppu_reg_write(Nes *n, int r, uint8_t v) {
    Ppu *p = &n->ppu;
    p->io_latch = v;
    switch (r) {
    case 0: {
        int was = p->ctrl & 0x80;
        p->ctrl = v;
        p->t = (uint16_t)((p->t & 0xF3FF) | ((v & 3) << 10));
        /* включение NMI во время vblank сразу даёт прерывание */
        if (!was && (v & 0x80) && (p->status & 0x80)) { n->nmi_line = 1; n->nmi_time = p->time; }
        break;
    }
    case 1: p->mask = v; break;
    case 3: p->oam_addr = v; break;
    case 4: p->oam[p->oam_addr++] = v; break;
    case 5:
        if (!p->w) { p->x = v & 7; p->t = (uint16_t)((p->t & 0xFFE0) | (v >> 3)); p->w = 1; }
        else { p->t = (uint16_t)((p->t & 0x8C1F) | ((v & 7) << 12) | ((v & 0xF8) << 2)); p->w = 0; }
        break;
    case 6:
        if (!p->w) { p->t = (uint16_t)((p->t & 0x80FF) | ((v & 0x3F) << 8)); p->w = 1; }
        else { p->t = (uint16_t)((p->t & 0xFF00) | v); p->v = p->t; p->w = 0; }
        break;
    case 7:
        ppu_mem_write(n, p->v, v);
        p->v = (uint16_t)(p->v + ((p->ctrl & 4) ? 32 : 1));
        break;
    }
}

/* ---------------------------------------------------------------- канал DMC */

static const int DMC_RATES[16] = { 428, 380, 340, 320, 286, 254, 226, 214, 190, 160, 142, 128, 106, 84, 72, 54 };

static void dmc_fetch(Nes *n, int is_load) {
    Dmc *d = &n->dmc;
    if (d->buffer_full || d->bytes_left == 0) return;
    d->buffer = nes_read(n, d->cur_addr);
    d->buffer_full = 1;
    d->stall += is_load ? d->stall_cost - 1 : d->stall_cost;  /* первая подкачка 3 такта, остальные 4 */
    d->cur_addr = d->cur_addr == 0xFFFF ? 0x8000 : (uint16_t)(d->cur_addr + 1);
    if (--d->bytes_left == 0) {
        if (d->loop) { d->cur_addr = d->sample_addr; d->bytes_left = d->sample_len; }
        else if (d->irq_enabled) d->irq_flag = 1;
    }
}

static void dmc_clock(Nes *n, int cycles) {
    Dmc *d = &n->dmc;
    d->timer -= cycles;
    while (d->timer <= 0) {
        d->timer += d->period;
        if (d->bits_left == 0) {
            d->bits_left = 8;
            if (d->buffer_full) { d->shift = d->buffer; d->buffer_full = 0; d->silence = 0; dmc_fetch(n, 0); }
            else d->silence = 1;
        }
        d->bits_left--;
    }
}

static void dmc_write(Nes *n, uint16_t a, uint8_t v) {
    Dmc *d = &n->dmc;
    switch (a) {
    case 0x4010:
        d->irq_enabled = v & 0x80; d->loop = v & 0x40; d->period = DMC_RATES[v & 15];
        if (!d->irq_enabled) d->irq_flag = 0;
        break;
    case 0x4012: d->sample_addr = (uint16_t)(0xC000 | (v << 6)); break;
    case 0x4013: d->sample_len = (v << 4) + 1; break;
    case 0x4015:
        d->irq_flag = 0;
        if (v & 0x10) {
            if (d->bytes_left == 0) { d->cur_addr = d->sample_addr; d->bytes_left = d->sample_len; dmc_fetch(n, 1); }
        } else d->bytes_left = 0;
        break;
    }
}

/* ---------------------------------------------------------------- шина процессора */

static int dma_cycles;   /* такты, украденные OAM DMA во время последней инструкции */

uint8_t nes_read(Nes *n, uint16_t a) {
    if (a < 0x2000) return n->ram[a & 0x7FF];
    if (a < 0x4000) { ppu_advance_to(n, cpu_now(n)); return ppu_reg_read(n, a & 7); }
    if (a == 0x4016) {
        uint8_t bit;
        if (n->pad_strobe) bit = n->pad_state & 1;
        else { bit = n->pad_shift & 1; n->pad_shift = (uint8_t)((n->pad_shift >> 1) | 0x80); }
        return (uint8_t)(0x40 | bit);
    }
    if (a == 0x4015) return (uint8_t)((n->dmc.irq_flag ? 0x80 : 0) | (n->dmc.bytes_left ? 0x10 : 0));
    if (a == 0x4017) return 0x40;
    if (a < 0x8000) return 0;
    if (a < 0xC000) return n->prg[n->bank * 0x4000 + (a - 0x8000)];
    return n->prg[(n->prg_banks - 1) * 0x4000 + (a - 0xC000)];
}

void nes_write(Nes *n, uint16_t a, uint8_t v) {
    if (a < 0x2000) { n->ram[a & 0x7FF] = v; return; }
    if (a < 0x4000) { ppu_advance_to(n, cpu_now(n)); ppu_reg_write(n, a & 7, v); return; }
    if (a == 0x4014) {
        uint16_t base = (uint16_t)(v << 8);
        for (int i = 0; i < 256; i++)
            n->ppu.oam[(uint8_t)(n->ppu.oam_addr + i)] = nes_read(n, (uint16_t)(base + i));
        dma_cycles += 513 + (int)(n->cpu.cycles & 1);
        return;
    }
    if (a == 0x4016) {
        n->pad_strobe = v & 1;
        if (n->pad_strobe) n->pad_shift = n->pad_state;
        else n->pad_shift = n->pad_state;
        return;
    }
    if (a >= 0x8000) { n->bank = v & (n->prg_banks - 1); return; }
    if (a == 0x4010 || a == 0x4012 || a == 0x4013 || a == 0x4015) { dmc_write(n, a, v); return; }
    /* остальной звук пока игнорируем */
}

static uint8_t bus_read(void *ctx, uint16_t a) { return nes_read((Nes *)ctx, a); }
static void bus_write(void *ctx, uint16_t a, uint8_t v) { nes_write((Nes *)ctx, a, v); }

/* ---------------------------------------------------------------- загрузка и сброс */

int nes_load_rom(Nes *n, const uint8_t *d, int size) {
    if (size < 16 || memcmp(d, "NES\x1A", 4) != 0) return -1;
    int prg16 = d[4], chr8 = d[5];
    int mapper = (d[6] >> 4) | (d[7] & 0xF0);
    int trainer = (d[6] & 4) ? 512 : 0;
    if (mapper != 2) return -2;                 /* ожидаем UNROM, как у TGL */
    if (16 + trainer + prg16 * 0x4000 + chr8 * 0x2000 > size) return -3;
    memset(n, 0, sizeof *n);
    n->prg = (uint8_t *)(d + 16 + trainer);
    n->prg_banks = prg16;
    for (int i = 0; i < 0x10000; i++) n->hook_bank[i] = -1;
    cpu_init(&n->cpu, bus_read, bus_write, n);
    return 0;
}

void nes_reset(Nes *n) {
    n->bank = 0;
    n->ppu.ctrl = n->ppu.mask = 0;
    n->ppu.w = 0;
    n->ppu.scanline = 0;
    n->ppu.dot = 0;
    n->ppu.time = 0;
    n->ppu.hit_x = -1;
    n->nmi_line = 0;
    n->ppu.skip_vblank = 1;
    memset(&n->dmc, 0, sizeof n->dmc);
    n->dmc.period = DMC_RATES[0];
    n->dmc.timer = 300;          /* фаза таймера DMC после перезагрузки (подобрана по записи) */
    n->dmc.stall_cost = 4;
    n->dmc.sample_addr = 0xC000;
    n->dmc.sample_len = 1;
    cpu_reset(&n->cpu);
}

void nes_set_hook(Nes *n, int bank, uint16_t addr, nes_hook_fn fn) {
    n->hooks[addr] = fn;
    n->hook_bank[addr] = bank;
}

/* ---------------------------------------------------------------- отрисовка строки */

static int rendering(Ppu *p) { return (p->mask & 0x18) != 0; }

static void render_line(Nes *n, int y) {
    Ppu *p = &n->ppu;
    uint8_t *out = &p->framebuffer[y * NES_SCREEN_W];
    uint8_t bg_opaque[NES_SCREEN_W];
    uint8_t backdrop = p->pal[0];

    memset(bg_opaque, 0, sizeof bg_opaque);
    for (int x = 0; x < NES_SCREEN_W; x++) out[x] = backdrop;

    if (p->mask & 0x08) {
        uint16_t v = p->v;
        int fine_y = (v >> 12) & 7;
        uint16_t pt = (p->ctrl & 0x10) ? 0x1000 : 0;
        int px = -(int)p->x;
        for (int tile = 0; tile < 33; tile++) {
            uint16_t nt_addr = 0x2000 | (v & 0x0FFF);
            uint8_t idx = ppu_mem_read(n, nt_addr);
            uint16_t at_addr = 0x23C0 | (v & 0x0C00) | ((v >> 4) & 0x38) | ((v >> 2) & 0x07);
            uint8_t at = ppu_mem_read(n, at_addr);
            int shift = ((v >> 4) & 4) | (v & 2);
            int palhi = ((at >> shift) & 3) << 2;
            uint8_t lo = n->chr[(pt + idx * 16 + fine_y) & 0x1FFF];
            uint8_t hi = n->chr[(pt + idx * 16 + fine_y + 8) & 0x1FFF];
            for (int b = 0; b < 8; b++, px++) {
                if (px < 0 || px >= NES_SCREEN_W) continue;
                int c = ((lo >> (7 - b)) & 1) | (((hi >> (7 - b)) & 1) << 1);
                if (px < 8 && !(p->mask & 0x02)) continue;
                if (c) { out[px] = p->pal[palhi | c]; bg_opaque[px] = 1; }
            }
            /* следующий тайл по X с переходом на соседний экран */
            if ((v & 0x1F) == 31) v = (uint16_t)((v & ~0x1F) ^ 0x0400);
            else v++;
        }
    }

    if (p->mask & 0x10) {
        int h = (p->ctrl & 0x20) ? 16 : 8;
        uint8_t drawn[NES_SCREEN_W];
        memset(drawn, 0, sizeof drawn);
        int count = 0;
        for (int i = 0; i < 64; i++) {
            const uint8_t *s = &p->oam[i * 4];
            int row = y - (s[0] + 1);
            if (row < 0 || row >= h) continue;
            if (++count > 8) { p->status |= 0x20; break; }
            uint8_t tile = s[1], attr = s[2];
            if (attr & 0x80) row = h - 1 - row;
            uint16_t addr;
            if (h == 16) {
                addr = (uint16_t)(((tile & 1) ? 0x1000 : 0) + (tile & 0xFE) * 16);
                if (row >= 8) { addr += 16; row -= 8; }
            } else {
                addr = (uint16_t)(((p->ctrl & 0x08) ? 0x1000 : 0) + tile * 16);
            }
            uint8_t lo = n->chr[(addr + row) & 0x1FFF];
            uint8_t hi = n->chr[(addr + row + 8) & 0x1FFF];
            for (int b = 0; b < 8; b++) {
                int px = s[3] + b;
                if (px >= NES_SCREEN_W) break;
                int bit = (attr & 0x40) ? b : 7 - b;
                int c = ((lo >> bit) & 1) | (((hi >> bit) & 1) << 1);
                if (!c) continue;
                if (px < 8 && !(p->mask & 0x04)) continue;
                if (i == 0 && bg_opaque[px] && px != 255 && (p->hit_x < 0 || px < p->hit_x)) p->hit_x = px;  /* попадание спрайта 0 */
                if (drawn[px]) continue;
                drawn[px] = 1;
                if ((attr & 0x20) && bg_opaque[px]) continue;              /* спрайт за фоном */
                out[px] = p->pal[0x10 | ((attr & 3) << 2) | c];
            }
        }
    }
}

static void inc_y(Ppu *p) {
    if ((p->v & 0x7000) != 0x7000) { p->v += 0x1000; return; }
    p->v &= (uint16_t)~0x7000;
    int y = (p->v >> 5) & 0x1F;
    if (y == 29) { y = 0; p->v ^= 0x0800; }
    else if (y == 31) y = 0;
    else y++;
    p->v = (uint16_t)((p->v & ~0x03E0) | (y << 5));
}

/* ---------------------------------------------------------------- тайминг
 * PPU продвигается по точкам (dot) и догоняет процессор перед каждым обращением
 * к своим регистрам и после каждой инструкции. Так прерывание кадра (NMI) приходит
 * на ту же инструкцию, что и в оригинале, — от этого зависит генератор случайных чисел игры. */

static void ppu_advance_to(Nes *n, uint64_t target) {
    Ppu *p = &n->ppu;
    while (p->time < target) {
        int sl = p->scanline, d = p->dot;
        int ren = rendering(p);
        if (sl < 240) {
            if (d == 1) {
                p->hit_x = -1;
                if (ren) render_line(n, sl);
                else for (int x = 0; x < NES_SCREEN_W; x++) p->framebuffer[sl * NES_SCREEN_W + x] = p->pal[0];
            } else if (p->hit_x >= 0 && d == p->hit_x + 2) {
                p->status |= 0x40;
                p->hit_x = -1;
            }
            if (ren && d == 256) inc_y(p);
            if (ren && d == 257) p->v = (uint16_t)((p->v & ~0x041F) | (p->t & 0x041F));
        } else if (sl == 241 && d == 1) {
            if (p->skip_vblank) p->skip_vblank = 0;
            else {
                p->status |= 0x80;
                if (p->ctrl & 0x80) { n->nmi_line = 1; n->nmi_time = p->time; }
            }
        } else if (sl == 261) {
            if (d == 1) p->status &= 0x1F;     /* сброс vblank, спрайта 0 и переполнения */
            if (ren && d == 257) p->v = (uint16_t)((p->v & ~0x041F) | (p->t & 0x041F));
            if (ren && d == 280) p->v = (uint16_t)((p->v & ~0x7BE0) | (p->t & 0x7BE0));
        }
        p->time++;
        d++;
        if (sl == 261 && d == 340 && ren && p->frame_odd) d = 341;   /* нечётный кадр короче на точку */
        if (d >= 341) {
            d = 0;
            sl = (sl + 1) % 262;
            if (sl == 0) p->frame_odd ^= 1;
            if (sl == 240) p->frame_ended = 1;
        }
        p->dot = d;
        p->scanline = sl;
    }
}

static void step(Nes *n) {
    /* NMI, выставленный до последнего такта предыдущей инструкции, срабатывает сейчас */
    if (n->nmi_line) {
        int64_t limit = (int64_t)(n->cpu.cycles - 1) * 3 + n->dot_offset + n->nmi_delay;
        if ((int64_t)n->nmi_time <= limit) { n->nmi_line = 0; cpu_nmi(&n->cpu); }
    }
    uint16_t pc = n->cpu.pc;
    nes_hook_fn h = n->hooks[pc];
    if (h && !n->cpu.nmi_pending &&
        (pc < 0x8000 || pc >= 0xC000 || n->hook_bank[pc] < 0 || n->hook_bank[pc] == n->bank)) {
        n->hook_calls++;
        h(n);
        n->cpu.cycles += (uint64_t)(n->hook_cycles[pc] > 0 ? n->hook_cycles[pc] : 50);
    } else {
        dma_cycles = 0;
        uint64_t before = n->cpu.cycles;
        cpu_step(&n->cpu);
        n->cpu.cycles += (uint64_t)dma_cycles;
        dmc_clock(n, (int)(n->cpu.cycles - before));
    }
    /* подкачка DMC отнимает такты у процессора */
    while (n->dmc.stall) {
        int st = n->dmc.stall;
        n->dmc.stall = 0;
        n->cpu.cycles += (uint64_t)st;
        dmc_clock(n, st);
    }
    n->cpu.irq_line = n->dmc.irq_flag;
    n->cpu.access_offset = 0;
    ppu_advance_to(n, cpu_now(n));
}

/* Кадр заканчивается в начале строки 240 (после видимой части), как событие
 * endFrame в Mesen: в этот момент снимаем память для сверки. */
void nes_run_frame(Nes *n) {
    n->ppu.frame_ended = 0;
    while (!n->ppu.frame_ended) step(n);
    n->frame++;
}
