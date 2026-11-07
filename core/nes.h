/* nes.h — «приставка» для порта: память, картридж UNROM, видеочип (PPU),
 * канал DMC, геймпад.
 *
 * Это не эмулятор общего назначения: он покрывает ровно то, что нужно The Guardian Legend
 * (маппер 2 / UNROM, CHR-RAM, вертикальное зеркалирование), и ничего лишнего.
 * Тайминг потактовый: каждое обращение процессора к шине продвигает остальные
 * устройства на один такт (см. bus_read/bus_write в nes.c).
 */
#ifndef NES_CORE_H
#define NES_CORE_H

#include <stdint.h>
#include "cpu6502.h"
#include "apu.h"

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
    int output;                 /* уровень на выходе (0..127) — только для звука */
    int dma_pending;            /* нужна подкачка байта (процессор остановится на ближайшем чтении) */
    int load_delay;             /* после записи в $4015 подкачка начинается через пару тактов */
    int dbg_fetches, dbg_irqs;  /* отладка: подкачки и прерывания за кадр */
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
    uint64_t frames_done;    /* сколько раз кончался кадр (растёт ровно в такт конца кадра) */
    uint64_t frame_end_time; /* точная точка (dot) конца кадра */
    int hit_x;               /* где на текущей строке будет попадание спрайта 0 (-1 = нет) */
    int frame_odd;
    int suppress_vbl;        /* чтение $2002 перед самым выставлением флага: в этом кадре флага не будет */
    int skip_vblank;         /* после перезагрузки первый vblank не выставляется (как в Mesen) */
    uint8_t framebuffer[NES_SCREEN_W * NES_SCREEN_H];  /* индексы палитры NES (0..63) */
} Ppu;

struct Nes {
    Cpu cpu;
    Ppu ppu;
    Dmc dmc;
    Apu apu;                 /* остальные звуковые каналы (на ход игры не влияют) */
    double audio_rate;       /* частота вывода звука (0 = 48000) */
    uint8_t ram[2048];
    uint8_t *prg;            /* PRG-ROM целиком */
    int prg_banks;           /* количество банков по 16 КБ */
    int bank;                /* текущий банк в $8000-$BFFF */
    uint8_t chr[8192];       /* CHR-RAM */
    uint8_t pad_state;       /* кнопки: A B Select Start Up Down Left Right = биты 0..7 */
    uint8_t pad_shift;
    int pad_strobe;
    uint64_t frame;

    int oam_dma_pending;     /* запись в $4014: копирование спрайтов на ближайшем чтении */
    uint8_t oam_dma_page;

    /* калибровка тайминга относительно Mesen */
    /* Время в мастер-тактах (как в Mesen): такт CPU = 12, точка PPU = 4.
     * На такте чтения обращение к шине идёт через 5 мастер-тактов от начала, на такте
     * записи — через 7; остаток такта проходит после обращения. */
    uint64_t master;
    int ppu_offset;          /* сдвиг PPU относительно CPU в мастер-тактах (калибровка по Mesen) */
    int dot_offset;          /* устарело: оставлено для совместимости с переменными окружения */
    int access_split;        /* устарело */
    int apu_phase;           /* какие такты считаются «get» для подкачки DMC (0/1) */
    int hit_delay;           /* через сколько точек после пикселя выставляется попадание спрайта 0 */
    int frame_end_dot;       /* в какой точке строки 240 Mesen сообщает конец кадра (калибровка) */
    int race_dot;            /* чтение $2002 в эту точку строки 241 «съедает» vblank (калибровка) */

    /* состояние ровно в момент конца кадра (для сверки с Mesen, который
     * снимает память посреди инструкции) */
    int fe_captured;
    uint16_t fe_pc;
    uint64_t fe_cycle;
    uint8_t fe_ram[2048];

    struct NesHooks *hooks;  /* переведённые процедуры (общие для всех копий состояния) */
    uint64_t hook_calls;     /* сколько раз сработали переведённые процедуры */
    int use_hooks;           /* 0 = всё исполняет эмулятор (эталон), 1 = переведённые процедуры включены */
    int xlat_bail;           /* переведённый код встретил то, что не умеет, и отдал управление эмулятору */
    int xlat_unwind;         /* переведённый код слишком долго не возвращается: свернуть всё до nes_step */
    uint64_t unwinds;        /* сколько раз переведённый код сворачивался по времени */
    int hook_depth;          /* вложенность перехватов (переведённый код может звать эмулятор и наоборот) */
    uint16_t hook_pc0;       /* адрес внешнего перехвата (для отчётов) */
    uint64_t hook_frames0;   /* номер кадра, когда начался внешний перехват */
    uint8_t shown[NES_SCREEN_W * NES_SCREEN_H];  /* картинка последнего законченного кадра */
    uint64_t stall_cycles;   /* такты, отнятые у процессора подкачками DMA */
    uint64_t emu_steps;      /* сколько инструкций исполнил эмулятор (не переведённый код) */
    /* вызывается ровно в такт конца кадра (до возврата из nes_run_frame) */
    void (*frame_end_fn)(Nes *n);
    /* вызывается перед каждой инструкцией, исполняемой эмулятором (для покрытия кода) */
    void (*trace_fn)(Nes *n);
    /* наблюдатель: вызывается перед инструкцией по адресу watch_pc, ничего не заменяя */
    uint16_t watch_pc;
    void (*watch_fn)(Nes *n);
    void *user;
};

/* Таблица перехватов вынесена из Nes, чтобы состояние приставки можно было
 * быстро копировать (сохранить/восстановить) — это нужно для сверки. */
typedef struct NesHooks {
    nes_hook_fn fixed[0x10000];       /* адреса вне $8000-$BFFF */
    nes_hook_fn banked[8][0x4000];    /* $8000-$BFFF: свой набор для каждого банка */
} NesHooks;

/* Загрузка ROM в формате iNES. Возвращает 0 при успехе. */
int  nes_load_rom(Nes *n, const uint8_t *data, int size);
void nes_reset(Nes *n);
/* Выполнить один кадр: до начала строки 240 (как endFrame в Mesen). */
void nes_run_frame(Nes *n);
/* Доступ к памяти без продвижения времени — для переведённого кода и отладки. */
uint8_t nes_read(Nes *n, uint16_t addr);
void    nes_write(Nes *n, uint16_t addr, uint8_t v);
/* Продвинуть время на заданное число тактов (переведённый код вызывает,
 * чтобы занять столько же тактов, сколько оригинал). */
void    nes_idle_cycles(Nes *n, int cycles);
/* Перехват: bank учитывается только для $8000-$BFFF. */
void    nes_set_hook(Nes *n, int bank, uint16_t addr, nes_hook_fn fn);
nes_hook_fn nes_find_hook(Nes *n, uint16_t addr);
/* Одна инструкция эмулятора (или переведённая процедура, если на этом адресе перехват). */
void    nes_step(Nes *n);
/* Один такт шины без процессора — для переведённого кода. is_write = 1 для такта записи
 * (на нём подкачка DMA не начинается, как у настоящего процессора). */
void    nes_bus_cycle(Nes *n, int is_write);
/* Палитра NES в RGB (64 цвета). */
extern const uint32_t NES_PALETTE[64];

#endif
