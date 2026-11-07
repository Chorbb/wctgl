/* apu.h — звук NES: два прямоугольных канала, треугольный, шум и смешивание с DMC.
 *
 * Канал DMC живёт в nes.c (его прерывание и подкачка влияют на ход игры); здесь — только
 * его громкость на выходе. Остальные каналы на ход игры не влияют: игра читает из $4015
 * лишь флаг прерывания DMC. Поэтому звук — пассивный выход: он считается по тактам
 * процессора и складывается в кольцевой буфер отсчётов.
 */
#ifndef TGL_APU_H
#define TGL_APU_H
#include <stdint.h>

typedef struct ApuEnv {          /* огибающая громкости */
    int start, divider, decay;
    int loop, constant, volume;  /* loop — он же «стоп» счётчика длины */
} ApuEnv;

typedef struct ApuPulse {
    ApuEnv env;
    int duty, step;              /* скважность (0..3) и позиция в последовательности (0..7) */
    int period, timer;           /* 11-битный период и счётчик (в тактах APU = 2 такта процессора) */
    int length, enabled;
    int sweep_on, sweep_period, sweep_neg, sweep_shift, sweep_div, sweep_reload;
    int ones_complement;         /* у первого канала вычитание «на единицу больше» */
} ApuPulse;

typedef struct ApuTri {
    int control, linear_load, linear, linear_reload;
    int period, timer, step;
    int length, enabled;
} ApuTri;

typedef struct ApuNoise {
    ApuEnv env;
    int mode, period, timer;
    uint16_t lfsr;
    int length, enabled;
} ApuNoise;

#define APU_RING 16384           /* отсчётов в кольцевом буфере (≈0,34 с при 48 кГц) */

typedef struct Apu {
    ApuPulse p1, p2;
    ApuTri tri;
    ApuNoise noise;
    int five_step;               /* $4017 бит 7: пятишаговый режим */
    int fc_cycle;                /* такт процессора внутри последовательности кадрового счётчика */
    int odd;                     /* чётность такта (прямоугольные и шум тикают каждые 2 такта) */

    /* вывод: усреднение по тактам процессора до частоты вывода, затем фильтры как у NES */
    double cycles_per_sample, acc_cycles, acc_sum;
    double hp1_prev_in, hp1_prev_out, hp2_prev_in, hp2_prev_out, lp_prev;
    double hp1_a, hp2_a, lp_a;
    int16_t ring[APU_RING];
    uint32_t ring_w, ring_r;     /* запись и чтение (растут бесконечно, индекс — по модулю) */
} Apu;

void apu_init(Apu *a, double output_rate);
void apu_set_rate(Apu *a, double output_rate);
void apu_write(Apu *a, uint16_t addr, uint8_t v);
/* биты 0-3 ответа на чтение $4015: какие каналы ещё звучат */
uint8_t apu_status(const Apu *a);
/* один такт процессора; dmc_level — текущий уровень DMC (0..127) */
void apu_clock(Apu *a, int dmc_level);
/* забрать до max отсчётов (16 бит, моно); возвращает сколько забрано */
int apu_read(Apu *a, int16_t *out, int max);

#endif
