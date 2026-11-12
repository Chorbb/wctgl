/* custom.c — новые враги (см. custom.h). */
#include "custom.h"
#include "rt.h"

/* ---------------------------------------------------------------- помощники */

/* такты своей логики: чтобы кадр занимал правдоподобное время */
static void tick(Nes *n, int cycles) { for (int i = 0; i < cycles; i++) nes_bus_cycle(n, 0); }

static void push(Nes *n, uint8_t v) { n->ram[0x100 | n->cpu.s] = v; n->cpu.s--; }
static uint8_t pull(Nes *n) { n->cpu.s++; return n->ram[0x100 | n->cpu.s]; }

/* Вызвать процедуру игры, как JSR: адрес возврата — ret (любой адрес вне кода игры),
 * исполнять (переводом или эмулятором), пока она не вернётся. 1 — вернулась как обычно. */
static int custom_call(Nes *n, uint16_t target, uint16_t ret) {
    uint16_t r = (uint16_t)(ret - 1);
    uint8_t s0 = n->cpu.s;
    push(n, (uint8_t)(r >> 8)); push(n, (uint8_t)r);
    tick(n, 6);
    n->cpu.pc = target;
    for (long guard = 0; guard < 2000000; guard++) {
        if (n->cpu.pc == ret && n->cpu.s == s0) return 1;
        if (n->cpu.jammed) return 0;
        nes_step(n);
    }
    return 0;
}

/* выйти из процедуры поведения, как RTS */
static void custom_rts(Nes *n) {
    uint8_t lo = pull(n), hi = pull(n);
    n->cpu.pc = (uint16_t)((lo | (hi << 8)) + 1);
    tick(n, 6);
}

/* свободная ячейка для пули: как $FA5B — ячейки 5..23 */
static int free_slot(Nes *n) {
    for (int y = 5; y < 24; y++) if (!n->ram[0x500 + y]) return y;
    return -1;
}

/* синус: 64 шага на период, амплитуда 127 */
static const int8_t SIN64[64] = {
    0, 12, 25, 37, 49, 60, 71, 81, 90, 98, 106, 112, 117, 122, 125, 126,
    127, 126, 125, 122, 117, 112, 106, 98, 90, 81, 71, 60, 49, 37, 25, 12,
    0, -12, -25, -37, -49, -60, -71, -81, -90, -98, -106, -112, -117, -122, -125, -126,
    -127, -126, -125, -122, -117, -112, -106, -98, -90, -81, -71, -60, -49, -37, -25, -12
};

/* ---------------------------------------------------------------- враг «синусоида»
 * Наследник врага типа 66 (основа должна быть из того же коридора: её графика уже загружена): его процедура ($ACE2) делает всё — заводит объект, графику,
 * движение, отрисовку, попадания, гибель. Перед ней подменяем скорость: вниз 1,25 точки
 * за кадр, по X — синус (до ±2 точек). Каждые 64 кадра — выстрел пулей типа 27, как у врагов
 * игры ($9F93). Пока выполняется процедура основы, тип объекта временно 28 — так она ведёт
 * себя ровно как у себя; после — снова наш тип.
 */
#define SINE_BASE_TYPE  66
#define SINE_BASE_AI    0xACE2
#define SINE_ADDR       0xBFD8
#define RET_ADDR        0xBFDB            /* адрес возврата для вызовов (вне кода игры) */

static void sine_flyer(Nes *n) {
    uint8_t x = n->cpu.x;
    uint8_t *phase = &n->custom_ram[0][x], *timer = &n->custom_ram[1][x];
    uint8_t mine = n->ram[0x500 + x];
    tick(n, 20);
    if (mine & 0x80) {                    /* уже заведён: своя траектория */
        *phase = (uint8_t)((*phase + 1) & 63);
        int vx = SIN64[*phase] * 2;       /* скорость в 1/128 точки → 8.8 со знаком */
        n->ram[0x608 + x] = (uint8_t)((vx >> 7) & 0xFF);
        n->ram[0x5F0 + x] = (uint8_t)((vx << 1) & 0xFF);
        n->ram[0x5D8 + x] = 1;            /* вниз 1,25 точки за кадр */
        n->ram[0x5C0 + x] = 0x40;
    }
    n->ram[0x500 + x] = (uint8_t)((mine & 0x80) | SINE_BASE_TYPE);
    n->cpu.x = x;
    if (!custom_call(n, SINE_BASE_AI, RET_ADDR)) return;   /* необычный выход — дальше эмулятор */
    uint8_t after = n->ram[0x500 + x];
    if ((after & 0x7F) == SINE_BASE_TYPE) n->ram[0x500 + x] = (uint8_t)((after & 0x80) | (mine & 0x7F));
    if (!(mine & 0x80) && (after & 0x80)) { *phase = (uint8_t)(x * 8); *timer = (uint8_t)(32 + x); }
    /* выстрел */
    if ((n->ram[0x500 + x] & 0x7F) == (mine & 0x7F) && (n->ram[0x500 + x] & 0x80) && --*timer == 0) {
        *timer = 64;
        int y = free_slot(n);
        if (y >= 0) {
            n->ram[0x500 + y] = 0x1B;
            n->ram[0x530 + y] = n->ram[0x530 + x];
            n->ram[0x518 + y] = n->ram[0x518 + x];
            n->ram[0x728 + y] = 0xFF;
            n->ram[0x680 + y] = n->ram[0x680 + x];
            n->ram[0x668 + y] = x;
        }
    }
    n->cpu.x = x;
    custom_rts(n);
}

/* ---------------------------------------------------------------- читы */
static int cheat_flags;

void custom_cheats(Nes *n, int flags) {
    cheat_flags = flags;
    if ((flags & 1) && n->ram[0x47]) { n->ram[0x48] = n->ram[0x47]; n->ram[0x49] = n->ram[0x47]; }
    /* все спецоружия, уровень 3: уровни по 2 бита на оружие 0-10 в $04AC-$04AE (читает $E498
     * банка 7, подбор оружия прибавляет 1 — банк 2 $86CD); биты 6-7 $04AE — другое, не трогаем */
    /* и бесконечные чипы: предел $4E/$4F не меньше 5000, чипы $4C/$4D = предел (так их ограничивает игра, $FCF2 банка 7) */
    if (flags & 4) {
        n->ram[0x4AC] = 0xFF; n->ram[0x4AD] = 0xFF; n->ram[0x4AE] |= 0x3F;
        if ((n->ram[0x4E] | n->ram[0x4F] << 8) < 5000) { n->ram[0x4E] = 5000 & 0xFF; n->ram[0x4F] = 5000 >> 8; }  /* предел — не меньше 5000 */
        n->ram[0x4C] = n->ram[0x4E]; n->ram[0x4D] = n->ram[0x4F];
    }
}

/* Убивать с одного попадания: банк 7 $D31C — объект X получает урон объекта Y
 * (hp(X) -= damage(Y), не ниже 0; $D31C-$D329). Если Y — снаряд игрока (ячейки 1-4; их
 * ищет $D348), здоровье X сразу 0, дальше игра как обычно ($D32A: гибель при 0). */
#define HIT_ADDR  0xD31C
#define HIT_AFTER 0xD32A
static int one_hit_here(Nes *n) {
    return (cheat_flags & 2) && n->cpu.y >= 1 && n->cpu.y <= 4;
}
static void one_hit(Nes *n) {
    n->ram[0x590 + n->cpu.x] = 0;
    n->cpu.a = 0;
    n->cpu.p = (uint8_t)((n->cpu.p & ~0x83) | 0x02);   /* как SBC с займом и STA 0: C=0, Z=1, N=0 */
    tick(n, 4 + 2 + 4 + 2 + 2 + 5);
    n->cpu.pc = HIT_AFTER;
}

/* ---------------------------------------------------------------- память модов */
uint8_t custom_mod_mem[MOD_MEM_SIZE];

void custom_mod_clear(void) { memset(custom_mod_mem, 0, sizeof custom_mod_mem); }

int custom_mod_write(int addr, const uint8_t *data, int len) {
    if (addr < 0 || len < 0 || addr + len > MOD_MEM_SIZE) return -3;
    for (int i = 0; i < len; i++) custom_mod_mem[addr + i] = data[i];
    return 0;
}

/* ---------------------------------------------------------------- свой узор комнаты
 * Банк 5 $8243 — начало наложения узора при постройке комнаты ($8155). Если у комнаты
 * ($52, $53) свой узор, кладём его клетки в сетку $740 и продолжаем с $82DD (после узора
 * игры), как будто у записи нет бита 4. Ловим только тогда — иначе перевод идёт как был. */
#define PAT_ADDR   0x8243
#define PAT_AFTER  0x82DD

static int room_pattern_of(Nes *n) {
    if (n->ram[0x30] & 0x20) return 0;                 /* внутренность особой комнаты */
    int x = n->ram[0x52], y = n->ram[0x53];
    if (x >= 24 || y >= 24) return 0;
    int k = custom_mod_mem[24 * y + x];
    return (k >= 1 && k <= MOD_PAT_SLOTS) ? k : 0;
}

static void room_pattern(Nes *n) {
    int k = room_pattern_of(n);
    const uint8_t *p = custom_mod_mem + 0x400 + 144 * (k - 1);
    for (int i = 0; i < 144; i++) if (p[i]) n->ram[0x760 + i] = p[i];
    tick(n, 400);
    n->cpu.pc = PAT_AFTER;
}

/* ---------------------------------------------------------------- свои комнаты
 * Банк 5 $80F4 — RTS в конце $8071: указатели на записи комнаты ($E4) и соседей (север $E0,
 * запад $E2, восток $E6, юг $E8) уже найдены по ROM. Если у какой-то из этих клеток своя
 * запись, указатель направляем на неё (окно $6000), затем выходим, как RTS. */
#define ROOMS_ADDR 0x80F4

static int room_rec(int x, int y) {
    if (x < 0 || y < 0 || x >= 24 || y >= 24) return 0;
    int k = custom_mod_mem[MOD_ROOM_IDX + 24 * y + x];
    return (k >= 1 && k <= MOD_WIN_SIZE / 16) ? k : 0;
}
static const int8_t NB[5][3] = { {0, -1, 0xE0}, {-1, 0, 0xE2}, {0, 0, 0xE4}, {1, 0, 0xE6}, {0, 1, 0xE8} };

static int rooms_here(Nes *n) {
    int x = n->ram[0x52], y = n->ram[0x53];
    for (int i = 0; i < 5; i++) if (room_rec(x + NB[i][0], y + NB[i][1])) return 1;
    return 0;
}

static void room_records(Nes *n) {
    int x = n->ram[0x52], y = n->ram[0x53];
    for (int i = 0; i < 5; i++) {
        int k = room_rec(x + NB[i][0], y + NB[i][1]);
        if (!k) continue;
        uint16_t a = (uint16_t)(0x6000 + 16 * (k - 1));
        n->ram[(uint8_t)NB[i][2]] = (uint8_t)a; n->ram[(uint8_t)(NB[i][2] + 1)] = (uint8_t)(a >> 8);
    }
    tick(n, 60);
    custom_rts(n);
}

/* Карта лабиринта (экран карты, банк 5 $882D-$88D5): игра перебирает записи подряд, указатель
 * $1A/$1B, клетка — $15 (X), $14 (Y). В $8853 (начало клетки, LDY #0) для клетки со своей
 * записью подменяем указатель на неё (окно $6000), в $88B9 (переход к следующей записи,
 * LDY #0) возвращаем указатель ROM — длину перехода игра берёт из записи ROM. */
#define MAP_CELL  0x8853
#define MAP_NEXT  0x88B9
static uint16_t map_saved; static int map_has;

static int map_cell_here(Nes *n) { return room_rec(n->ram[0x15], n->ram[0x14]) != 0; }
static void ldy0(Nes *n, uint16_t next) {
    n->cpu.y = 0;
    n->cpu.p = (uint8_t)((n->cpu.p & ~0x80) | 0x02);
    tick(n, 2);
    n->cpu.pc = next;
}
static void map_cell(Nes *n) {
    int k = room_rec(n->ram[0x15], n->ram[0x14]);
    map_saved = (uint16_t)(n->ram[0x1A] | (n->ram[0x1B] << 8)); map_has = 1;
    uint16_t a = (uint16_t)(0x6000 + 16 * (k - 1));
    n->ram[0x1A] = (uint8_t)a; n->ram[0x1B] = (uint8_t)(a >> 8);
    ldy0(n, MAP_CELL + 2);
}
static void map_next(Nes *n) {
    n->ram[0x1A] = (uint8_t)map_saved; n->ram[0x1B] = (uint8_t)(map_saved >> 8); map_has = 0;
    ldy0(n, MAP_NEXT + 2);
}

const CustomInfo custom_table[] = {
    { 6, SINE_ADDR, sine_flyer, "sine_flyer",
      "наследник типа 66: синусоида вниз, выстрел раз в 64 кадра" },
};
const int custom_count = (int)(sizeof custom_table / sizeof custom_table[0]);

static nes_hook_fn custom_lookup(Nes *n, uint16_t addr) {
    if (addr == PAT_ADDR && (n->bank & 7) == 5) return room_pattern_of(n) ? room_pattern : 0;
    if (addr == ROOMS_ADDR && (n->bank & 7) == 5) return rooms_here(n) ? room_records : 0;
    if (addr == HIT_ADDR) return one_hit_here(n) ? one_hit : 0;
    if (addr == MAP_CELL && (n->bank & 7) == 5) return map_cell_here(n) ? map_cell : 0;
    if (addr == MAP_NEXT && (n->bank & 7) == 5) return map_has ? map_next : 0;
#ifdef CUSTOM_DEBUG
    if (addr == 0xBFD8) { extern int printf(const char *, ...); printf("lookup BFD8 bank %d\n", n->bank); }
#endif
    for (int i = 0; i < custom_count; i++) {
        const CustomInfo *c = &custom_table[i];
        if (c->addr != addr) continue;
        if (addr >= 0x8000 && addr < 0xC000 && c->bank != (n->bank & 7)) continue;
        return c->fn;
    }
    return 0;
}

void custom_install(Nes *n) { n->custom_find = custom_lookup; }

/* ---------------------------------------------------------------- арт мода (см. custom.h) */
typedef struct {
    uint8_t type, frame;
    int8_t ax, ay;
    uint16_t w, h;
    uint32_t off;                       /* в art_pool, w*h*4 */
    uint8_t nhide;
    int8_t hide[ART_HIDE_MAX][2];
} ArtFrame;
static ArtFrame art[ART_MAX];
int custom_art_count;
static uint8_t art_pool[ART_POOL];
static uint32_t art_used;
static uint8_t art_obj[24][4];          /* тип, кадр, X, Y — в момент копирования спрайтов */
static int16_t art_of[24];              /* какая картинка у объекта или -1 */
static int art_clip = 240;              /* рисовать только выше этой строки: панель внизу экрана */

void custom_art_clear(void) { custom_art_count = 0; art_used = 0; for (int i = 0; i < 24; i++) art_of[i] = -1; }

int custom_art_add(int type, int frame, int ax, int ay, int w, int h, const uint8_t *rgba, const int8_t *hide, int nhide) {
    if (custom_art_count >= ART_MAX || w <= 0 || h <= 0 || w > 256 || h > 240 || nhide < 0 || nhide > ART_HIDE_MAX) return -1;
    uint32_t sz = (uint32_t)(w * h * 4);
    if (art_used + sz > ART_POOL) return -2;
    ArtFrame *a = &art[custom_art_count];
    a->type = (uint8_t)type; a->frame = (uint8_t)frame; a->ax = (int8_t)ax; a->ay = (int8_t)ay;
    a->w = (uint16_t)w; a->h = (uint16_t)h; a->off = art_used; a->nhide = (uint8_t)nhide;
    for (uint32_t i = 0; i < sz; i++) art_pool[art_used + i] = rgba[i];
    for (int i = 0; i < nhide; i++) { a->hide[i][0] = hide[2 * i]; a->hide[i][1] = hide[2 * i + 1]; }
    art_used += sz;
    return custom_art_count++;
}

void custom_art_dma(Nes *n) {
    if (!custom_art_count) return;
    /* спрайт 0 — метка, где игра переключает экран на панель (Y=$BF → строка 192): ниже него
     * игровое поле кончается, спрайты игры туда не попадают — и арт обрезаем по ней */
    art_clip = n->ppu.oam[0] < 0xEF ? n->ppu.oam[0] + 1 : 240;
    for (int k = 0; k < 24; k++) {
        art_of[k] = -1;
        uint8_t t = n->ram[0x500 + k];
        art_obj[k][0] = t & 0x7F; art_obj[k][1] = n->ram[0x548 + k];
        art_obj[k][2] = n->ram[0x530 + k]; art_obj[k][3] = n->ram[0x518 + k];
        if (!t) continue;
        for (int i = 0; i < custom_art_count; i++)
            if (art[i].type == art_obj[k][0] && art[i].frame == art_obj[k][1]) { art_of[k] = (int16_t)i; break; }
        if (art_of[k] < 0) continue;
        const ArtFrame *a = &art[art_of[k]];
        int found = 0;
        for (int o = 1; o < 64; o++) {                 /* спрайт 0 не трогаем */
            uint8_t *e = n->ppu.oam + o * 4;
            if (e[0] >= 0xEF) continue;
            int dx = (int8_t)(uint8_t)(e[3] - art_obj[k][2]), dy = (int8_t)(uint8_t)(e[0] - art_obj[k][3]);
            for (int h = 0; h < a->nhide; h++)
                if (a->hide[h][0] == dx && a->hide[h][1] == dy) { e[0] = 0xF8; found++; break; }
        }
        /* игра этот кадр не рисовала (экран паузы, мерцание, объект за краем) — и мы не рисуем */
        if (!found) art_of[k] = -1;
    }
}

void custom_art_draw(uint8_t *rgba) {
    if (!custom_art_count) return;
    for (int k = 0; k < 24; k++) {
        if (art_of[k] < 0) continue;
        const ArtFrame *a = &art[art_of[k]];
        int x0 = art_obj[k][2] + a->ax, y0 = art_obj[k][3] + a->ay + 1;
        const uint8_t *src = art_pool + a->off;
        for (int y = 0; y < a->h; y++) {
            int sy = y0 + y; if (sy < 0 || sy >= art_clip) continue;
            for (int x = 0; x < a->w; x++) {
                int sx = x0 + x; if (sx < 0 || sx >= 256) continue;
                const uint8_t *p = src + (y * a->w + x) * 4;
                if (p[3] < 128) continue;
                uint8_t *d = rgba + (sy * 256 + sx) * 4;
                d[0] = p[0]; d[1] = p[1]; d[2] = p[2]; d[3] = 255;
            }
        }
    }
}
