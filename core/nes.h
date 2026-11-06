/* nes.h — «приставка» для порта: память, картридж UNROM, видеочип (PPU), геймпад.
 *
 * Это не эмулятор общего назначения: он покрывает ровно то, что нужно The Guardian Legend
 * (маппер 2 / UNROM, CHR-RAM, вертикальное зеркалирование), и ничего лишнего.
 */
#ifndef NES_CORE_H
#define NES_CORE_H

#include <stdint.h>
#include "cpu6502.h"

#define NES_SCREEN_W 256
#define NES_SCREEN_H 240

typedef struct Nes Nes;

/* Переведённая процедура: вызывается вместо оригинального кода по адресу.
 * Должна оставить процессор в том состоянии, в каком его оставил бы оригинал
 * (обычно — выполнить RTS). */
typedef void (*nes_hook_fn)(Nes *n);

/* Канал DMC звукового чипа. TGL использует его прерывание, чтобы отделить
 * нижнюю панель HUD от игрового поля, поэтому тайминг важен. Сам звук пока не выводим. */
typedef struct Dmc {
    int irq_enabled, loop, irq_flag;
    int period, timer;          /* период в тактах CPU и счётчик */
    uint16_t sample_addr, cur_addr;
    int sample_len, bytes_left;
    int buffer_full;
    uint8_t buffer, shift;
    int bits_left;
    int silence;
    int stall;                  /* такты, которые нужно отнять у процессора на подкачку */
    int stall_cost;             /* сколько тактов стоит одна подкачка (калибровка, обычно 4) */
} Dmc;

typedef struct Ppu {
    uint8_t ctrl, mask, status, oam_addr;
    uint16_t v, t;           /* «loopy»-регистры адреса и прокрутки */
    uint8_t x, w;            /* тонкая прокрутка по X и переключатель записи */
    uint8_t read_buffer;
    uint8_t io_latch;        /* «открытая шина» PPU: последнее значение на шине регистров */
    uint8_t oam[256];
    uint8_t nt[2048];        /* память фона (2 экрана) */
    uint8_t pal[32];
    int scanline;            /* 0..261 */
    int dot;                 /* 0..340 */
    uint64_t time;           /* сколько точек (dot) прошло с перезагрузки */
    int frame_ended;         /* вошли в строку 240: кадр закончен */
    int hit_x;               /* где на текущей строке будет попадание спрайта 0 (-1 = нет) */
    int frame_odd;
    int skip_vblank;         /* после перезагрузки первый vblank не выставляется (как в Mesen) */
    uint8_t framebuffer[NES_SCREEN_W * NES_SCREEN_H];  /* индексы палитры NES (0..63) */
} Ppu;

struct Nes {
    Cpu cpu;
    Ppu ppu;
    uint8_t ram[2048];
    uint8_t *prg;            /* PRG-ROM целиком */
    int prg_banks;           /* количество банков по 16 КБ */
    int bank;                /* текущий банк в $8000-$BFFF */
    uint8_t chr[8192];       /* CHR-RAM */
    Dmc dmc;
    uint8_t pad_state;       /* кнопки: A B Select Start Up Down Left Right = биты 0..7 */
    uint8_t pad_shift;
    int pad_strobe;
    uint64_t frame;
    int dot_offset;          /* сдвиг PPU относительно CPU в точках (калибровка по Mesen) */
    int nmi_delay;           /* поправка момента срабатывания NMI в точках (калибровка) */
    int nmi_line;            /* NMI выставлен и ещё не обработан */
    uint64_t nmi_time;       /* когда выставлен NMI (в точках PPU) */
    nes_hook_fn hooks[0x10000];       /* перехват по адресу; NULL = нет */
    int hook_bank[0x10000];  /* к какому банку относится перехват в $8000-$BFFF (-1 = любой) */
    uint64_t hook_calls;     /* сколько раз сработали переведённые процедуры */
    int hook_cycles[0x10000];/* сколько тактов занимает оригинал (для тайминга) */
};

/* Загрузка ROM в формате iNES. Возвращает 0 при успехе. */
int  nes_load_rom(Nes *n, const uint8_t *data, int size);
void nes_reset(Nes *n);
/* Выполнить один кадр: с начала видимой части до начала следующей. */
void nes_run_frame(Nes *n);
/* Вспомогательные функции для переведённого кода. */
uint8_t nes_read(Nes *n, uint16_t addr);
void    nes_write(Nes *n, uint16_t addr, uint8_t v);
void    nes_set_hook(Nes *n, int bank, uint16_t addr, nes_hook_fn fn);
/* Палитра NES в RGB (64 цвета). */
extern const uint32_t NES_PALETTE[64];

#endif
