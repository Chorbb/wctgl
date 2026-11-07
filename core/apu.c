/* apu.c — звук NES (см. apu.h). Таблицы и поведение — по описанию 2A03 (NTSC). */
#include "apu.h"
#include "rt.h"

#define CPU_HZ 1789772.7272727

static const uint8_t LENGTH_TABLE[32] = {
    10, 254, 20, 2, 40, 4, 80, 6, 160, 8, 60, 10, 14, 12, 26, 14,
    12, 16, 24, 18, 48, 20, 96, 22, 192, 24, 72, 26, 16, 28, 32, 30
};
static const uint8_t DUTY[4][8] = {
    { 0, 1, 0, 0, 0, 0, 0, 0 }, { 0, 1, 1, 0, 0, 0, 0, 0 },
    { 0, 1, 1, 1, 1, 0, 0, 0 }, { 1, 0, 0, 1, 1, 1, 1, 1 }
};
static const uint8_t TRI_SEQ[32] = {
    15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0,
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15
};
static const uint16_t NOISE_PERIOD[16] = {
    4, 8, 16, 32, 64, 96, 128, 160, 202, 254, 380, 508, 762, 1016, 2034, 4068
};

void apu_quarter_half(Apu *a, int half);

/* смешивание как у NES (нелинейное), заранее в таблицах */
static float pulse_mix[31], tnd_mix[203];
static int tables_ready;

static void make_tables(void) {
    for (int i = 0; i < 31; i++) pulse_mix[i] = i ? (float)(95.52 / (8128.0 / i + 100.0)) : 0.0f;
    for (int i = 0; i < 203; i++) tnd_mix[i] = i ? (float)(163.67 / (24329.0 / i + 100.0)) : 0.0f;
    tables_ready = 1;
}

/* коэффициент однополюсного фильтра для частоты среза fc при частоте вывода fs */
static double rc_coef(double fc, double fs) {
    const double pi = 3.14159265358979;
    double rc = 1.0 / (2.0 * pi * fc), dt = 1.0 / fs;
    return rc / (rc + dt);
}

void apu_set_rate(Apu *a, double rate) {
    if (rate < 8000) rate = 8000;
    a->cycles_per_sample = CPU_HZ / rate;
    a->hp1_a = rc_coef(90.0, rate);      /* фильтры выхода NES: ВЧ 90 Гц, ВЧ 440 Гц, НЧ 14 кГц */
    a->hp2_a = rc_coef(440.0, rate);
    {
        const double pi = 3.14159265358979;
        double rc = 1.0 / (2.0 * pi * 14000.0), dt = 1.0 / rate;
        a->lp_a = dt / (rc + dt);
    }
}

void apu_init(Apu *a, double rate) {
    if (!tables_ready) make_tables();
    memset(a, 0, sizeof *a);
    a->noise.lfsr = 1;
    a->p1.ones_complement = 1;
    apu_set_rate(a, rate);
}

/* ---------------------------------------------------------------- регистры */

static void env_write(ApuEnv *e, uint8_t v) {
    e->loop = (v >> 5) & 1;
    e->constant = (v >> 4) & 1;
    e->volume = v & 15;
}

static void pulse_write(ApuPulse *p, int reg, uint8_t v) {
    switch (reg) {
    case 0: p->duty = v >> 6; env_write(&p->env, v); break;
    case 1:
        p->sweep_on = (v >> 7) & 1; p->sweep_period = (v >> 4) & 7;
        p->sweep_neg = (v >> 3) & 1; p->sweep_shift = v & 7; p->sweep_reload = 1;
        break;
    case 2: p->period = (p->period & 0x700) | v; break;
    case 3:
        p->period = (p->period & 0xFF) | ((v & 7) << 8);
        if (p->enabled) p->length = LENGTH_TABLE[v >> 3];
        p->step = 0;
        p->env.start = 1;
        break;
    }
}

void apu_write(Apu *a, uint16_t addr, uint8_t v) {
    if (addr >= 0x4000 && addr <= 0x4003) { pulse_write(&a->p1, addr & 3, v); return; }
    if (addr >= 0x4004 && addr <= 0x4007) { pulse_write(&a->p2, addr & 3, v); return; }
    switch (addr) {
    case 0x4008: a->tri.control = (v >> 7) & 1; a->tri.linear_load = v & 0x7F; break;
    case 0x400A: a->tri.period = (a->tri.period & 0x700) | v; break;
    case 0x400B:
        a->tri.period = (a->tri.period & 0xFF) | ((v & 7) << 8);
        if (a->tri.enabled) a->tri.length = LENGTH_TABLE[v >> 3];
        a->tri.linear_reload = 1;
        break;
    case 0x400C: env_write(&a->noise.env, v); break;
    case 0x400E: a->noise.mode = (v >> 7) & 1; a->noise.period = NOISE_PERIOD[v & 15]; break;
    case 0x400F:
        if (a->noise.enabled) a->noise.length = LENGTH_TABLE[v >> 3];
        a->noise.env.start = 1;
        break;
    case 0x4015:
        a->p1.enabled = v & 1;    if (!a->p1.enabled) a->p1.length = 0;
        a->p2.enabled = (v >> 1) & 1; if (!a->p2.enabled) a->p2.length = 0;
        a->tri.enabled = (v >> 2) & 1; if (!a->tri.enabled) a->tri.length = 0;
        a->noise.enabled = (v >> 3) & 1; if (!a->noise.enabled) a->noise.length = 0;
        break;
    case 0x4017:
        a->five_step = (v >> 7) & 1;
        a->fc_cycle = 0;
        if (a->five_step) apu_quarter_half(a, 1);   /* в пятишаговом режиме — сразу четверть и половина кадра */
        break;
    }
}

uint8_t apu_status(const Apu *a) {
    return (uint8_t)((a->p1.length > 0) | ((a->p2.length > 0) << 1) | ((a->tri.length > 0) << 2) |
                     ((a->noise.length > 0) << 3));
}

/* ---------------------------------------------------------------- кадровый счётчик */

static void env_clock(ApuEnv *e) {
    if (e->start) { e->start = 0; e->decay = 15; e->divider = e->volume; return; }
    if (e->divider > 0) { e->divider--; return; }
    e->divider = e->volume;
    if (e->decay > 0) e->decay--;
    else if (e->loop) e->decay = 15;
}

static int sweep_target(const ApuPulse *p) {
    int c = p->period >> p->sweep_shift;
    if (p->sweep_neg) return p->period - c - (p->ones_complement ? 1 : 0);
    return p->period + c;
}

static void sweep_clock(ApuPulse *p) {
    int t = sweep_target(p);
    if (p->sweep_div == 0 && p->sweep_on && p->sweep_shift > 0 && p->period >= 8 && t <= 0x7FF)
        p->period = t;
    if (p->sweep_div == 0 || p->sweep_reload) { p->sweep_div = p->sweep_period; p->sweep_reload = 0; }
    else p->sweep_div--;
}

void apu_quarter_half(Apu *a, int half) {
    env_clock(&a->p1.env);
    env_clock(&a->p2.env);
    env_clock(&a->noise.env);
    if (a->tri.linear_reload) a->tri.linear = a->tri.linear_load;
    else if (a->tri.linear > 0) a->tri.linear--;
    if (!a->tri.control) a->tri.linear_reload = 0;
    if (!half) return;
    if (a->p1.length > 0 && !a->p1.env.loop) a->p1.length--;
    if (a->p2.length > 0 && !a->p2.env.loop) a->p2.length--;
    if (a->tri.length > 0 && !a->tri.control) a->tri.length--;
    if (a->noise.length > 0 && !a->noise.env.loop) a->noise.length--;
    sweep_clock(&a->p1);
    sweep_clock(&a->p2);
}

static void frame_counter(Apu *a) {
    /* шаги в тактах процессора (NTSC) */
    int c = ++a->fc_cycle;
    if (!a->five_step) {
        if (c == 7457) apu_quarter_half(a, 0);
        else if (c == 14913) apu_quarter_half(a, 1);
        else if (c == 22371) apu_quarter_half(a, 0);
        else if (c == 29829) { apu_quarter_half(a, 1); a->fc_cycle = 0; }
    } else {
        if (c == 7457) apu_quarter_half(a, 0);
        else if (c == 14913) apu_quarter_half(a, 1);
        else if (c == 22371) apu_quarter_half(a, 0);
        else if (c == 37281) { apu_quarter_half(a, 1); a->fc_cycle = 0; }
    }
}

/* ---------------------------------------------------------------- выход каналов */

static int env_out(const ApuEnv *e) { return e->constant ? e->volume : e->decay; }

static int pulse_out(const ApuPulse *p) {
    if (!p->length || p->period < 8 || (!p->sweep_neg && sweep_target(p) > 0x7FF)) return 0;
    if (!DUTY[p->duty][p->step]) return 0;
    return env_out(&p->env);
}

static void pulse_tick(ApuPulse *p) {
    if (p->timer == 0) { p->timer = p->period; p->step = (p->step + 1) & 7; }
    else p->timer--;
}

void apu_clock(Apu *a, int dmc_level) {
    frame_counter(a);

    /* треугольный — каждый такт процессора; стоит, если счётчики на нуле */
    if (a->tri.timer == 0) {
        a->tri.timer = a->tri.period;
        if (a->tri.length > 0 && a->tri.linear > 0) a->tri.step = (a->tri.step + 1) & 31;
    } else a->tri.timer--;

    a->odd ^= 1;
    if (a->odd) {                  /* такт APU: прямоугольные */
        pulse_tick(&a->p1);
        pulse_tick(&a->p2);
    }
    /* шум: период в таблице — в тактах процессора */
    if (a->noise.timer == 0) {
        a->noise.timer = a->noise.period ? a->noise.period - 1 : 0;
        int bit = (a->noise.lfsr ^ (a->noise.lfsr >> (a->noise.mode ? 6 : 1))) & 1;
        a->noise.lfsr = (uint16_t)((a->noise.lfsr >> 1) | (bit << 14));
    } else a->noise.timer--;

    int p = pulse_out(&a->p1) + pulse_out(&a->p2);
    /* треугольный на сверхвысоких периодах (<2) даёт ультразвук — как многие эмуляторы, глушим */
    int t = a->tri.period >= 2 ? TRI_SEQ[a->tri.step] : 7;
    int nz = (a->noise.length > 0 && !(a->noise.lfsr & 1)) ? env_out(&a->noise.env) : 0;
    double s = pulse_mix[p] + tnd_mix[3 * t + 2 * nz + (dmc_level & 127)];

    a->acc_sum += s;
    a->acc_cycles += 1.0;
    if (a->acc_cycles >= a->cycles_per_sample) {
        double x = a->acc_sum / a->acc_cycles;
        a->acc_cycles -= a->cycles_per_sample;
        a->acc_sum = 0;
        /* фильтры выхода */
        double y1 = a->hp1_a * (a->hp1_prev_out + x - a->hp1_prev_in);
        a->hp1_prev_in = x; a->hp1_prev_out = y1;
        double y2 = a->hp2_a * (a->hp2_prev_out + y1 - a->hp2_prev_in);
        a->hp2_prev_in = y1; a->hp2_prev_out = y2;
        a->lp_prev += a->lp_a * (y2 - a->lp_prev);
        double v = a->lp_prev * 32767.0 * 1.6;
        if (v > 32767) v = 32767;
        if (v < -32768) v = -32768;
        if (a->ring_w - a->ring_r >= APU_RING) a->ring_r++;   /* никто не читает — старое выбрасываем */
        a->ring[a->ring_w % APU_RING] = (int16_t)v;
        a->ring_w++;
    }
}

int apu_read(Apu *a, int16_t *out, int max) {
    int n = 0;
    while (n < max && a->ring_r != a->ring_w) out[n++] = a->ring[a->ring_r++ % APU_RING];
    return n;
}
