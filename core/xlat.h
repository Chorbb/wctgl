/* xlat.h — среда выполнения переведённого кода.
 *
 * Переводчик (tools/xlat.py) превращает процедуру 6502 в функцию C, где каждая
 * инструкция — строка C. Чтобы игра вела себя точно как оригинал, переведённый код:
 *   - держит регистры прямо в n->cpu (A, X, Y, S, P) и стек — в оперативной памяти;
 *   - после каждой инструкции отсчитывает её такты по шаблону шины (какие такты —
 *     чтение, какие — запись), поэтому подкачки DMA и опрос прерываний идут как у процессора;
 *   - принимает прерывание на той же границе инструкции, что и оригинал: кладёт в стек
 *     адрес следующей инструкции, исполняет обработчик и продолжает после RTI;
 *   - инструкции, обращающиеся к железу ($2000-$401F), отдаёт эмулятору — так их
 *     тайминг совпадает до такта;
 *   - то, чего переводчик не умеет (переходы по таблице, трюки со стеком),
 *     отдаёт эмулятору целиком (xlat_bail).
 *
 * Сгенерированный код зависит от ROM, поэтому создаётся при сборке в build/gen
 * и в репозиторий не попадает.
 */
#ifndef TGL_XLAT_H
#define TGL_XLAT_H

#include "nes.h"

/* Шаблон шины инструкции: число тактов и маска тактов записи (бит i = такт i — запись). */
void xlat_tick(Nes *n, int count, unsigned wmask);
/* Опрос прерываний после инструкции; next — адрес следующей инструкции (туда вернётся RTI).
 * Возвращает 1, если переведённому коду надо выйти (управление у эмулятора). */
int  xlat_check(Nes *n, uint16_t next);
/* Такты + опрос: обычный конец инструкции. */
int  xlat_cycles(Nes *n, int count, unsigned wmask, uint16_t next);
/* Переход JMP на адрес target (такты уже отсчитаны): если там переведённая процедура —
 * вызвать её (она вернётся туда же, куда вернулась бы оригинальная), иначе отдать эмулятору. */
void xlat_jump(Nes *n, uint16_t target);
/* Условный переход: 2 такта, +1 если взят, +1 если ещё и сменил страницу.
 * Взятый переход без смены страницы не замечает новый IRQ (как у 6502). */
int  xlat_branch(Nes *n, int taken, uint16_t from_next, uint16_t to);
/* Инструкцию по адресу pc (без перехода, следующая — next) исполняет эмулятор:
 * обращения к железу так попадают точно в свой такт. Если после неё пришло прерывание,
 * оно отрабатывается до возврата в next. */
int  xlat_io(Nes *n, uint16_t pc, uint16_t next);
/* Вызов процедуры по JSR: адрес возврата уже в стеке, такты JSR отсчитаны.
 * Если процедура переведена — вызывается её функция, иначе её исполняет эмулятор.
 * Возвращает 1, если вернулись как обычно (в ret_next), 0 — если надо отдать управление. */
int xlat_call(Nes *n, uint16_t target, uint16_t ret_next);
/* Дальний вызов (JSR на переходник между банками с параметрами после JSR): адрес возврата
 * в стеке, такты JSR отсчитаны. Переходник исполняет эмулятор; вызываемая процедура, если
 * она переведена, подхватывается по адресу входа. Возврат — в after (за параметрами). */
int xlat_far(Nes *n, uint16_t after);
/* Отдать управление эмулятору с адреса pc (остаток процедуры исполнит он). */
void xlat_bail_to(Nes *n, uint16_t pc);

/* Память без продвижения времени. Обычные адреса — напрямую, железо — через шину. */
static inline uint8_t xlat_rd(Nes *n, uint16_t a) {
    if (a < 0x2000) return n->ram[a & 0x7FF];
    return nes_read(n, a);
}
static inline void xlat_wr(Nes *n, uint16_t a, uint8_t v) {
    if (a < 0x2000) { n->ram[a & 0x7FF] = v; return; }
    nes_write(n, a, v);
}

/* Переведённый код не должен держать управление дольше нескольких кадров (иначе кадр
 * не вернётся в Godot): тогда всё сворачивается до nes_step, остальное доделает эмулятор.
 * Вызывается в ожиданиях и на переходах назад. 1 = надо выходить. */
#define XLAT_MAX_FRAMES 3
static inline int xlat_stop(Nes *n) {
    if (!n->xlat_unwind && n->ppu.frames_done - n->hook_frames0 > XLAT_MAX_FRAMES) { n->xlat_unwind = 1; n->unwinds++; }
    if (n->xlat_unwind) { n->xlat_bail = 1; return 1; }
    return 0;
}

/* ---- сокращения для сгенерированного кода ---- */
#define XA  (n->cpu.a)
#define XX  (n->cpu.x)
#define XY  (n->cpu.y)
#define XS  (n->cpu.s)
#define XP  (n->cpu.p)
#define XRD(a)    xlat_rd(n, (uint16_t)(a))
#define XWR(a, v) xlat_wr(n, (uint16_t)(a), (uint8_t)(v))
#define XZP(a)    (n->ram[(a) & 0xFF])
#define XPUSH(v)  do { n->ram[0x100 | XS] = (uint8_t)(v); XS--; } while (0)
#define XPULL()   (XS++, n->ram[0x100 | XS])
#define XZN(v)    do { uint8_t z_ = (uint8_t)(v); XP = (uint8_t)((XP & 0x7D) | (z_ ? 0 : 0x02) | (z_ & 0x80)); } while (0)
#define XCROSS(a, b) ((((a) ^ (b)) & 0xFF00) != 0)
/* каждая из них возвращает 1, если пришлось отдать управление (прерывание ушло не туда,
 * слишком долго) — тогда переведённая функция сразу выходит */
#define XCYC(cnt, wm, next) if (xlat_cycles(n, (cnt), (wm), (uint16_t)(next))) return
#define XBR(...)  if (xlat_branch(n, __VA_ARGS__)) return
#define XIOX(...) if (xlat_io(n, __VA_ARGS__)) return
#define XCHK(at)  if (xlat_check(n, (uint16_t)(at))) return
/* точка для модов (routines.txt: «hook банк адрес»): если ядро повесило туда свою процедуру
 * (custom_find), отдаём управление эмулятору — он вызовет её; иначе перевод идёт дальше */
#define XHOOK(at) do { if (n->custom_find && n->custom_find(n, (at))) { xlat_bail_to(n, (at)); return; } } while (0)

/* трассировка (для поиска расхождений): перед каждой инструкцией, как у эмулятора */
#ifdef XLAT_NOTRACE
#define XT(at) ((void)0)
#else
#define XT(at) do { if (n->trace_fn) { n->cpu.pc = (at); n->trace_fn(n); } } while (0)
#endif
#define XIO(a)    ((uint16_t)((a) - 0x2000) < 0x2020)   /* адрес железа: $2000-$401F */

static inline void xlat_adc(Nes *n, uint8_t m) {
    unsigned s = XA + m + (XP & 1);
    XP = (uint8_t)(XP & ~0x41);
    if (s > 0xFF) XP |= 0x01;
    if (~(XA ^ m) & (XA ^ s) & 0x80) XP |= 0x40;
    XA = (uint8_t)s;
    XZN(XA);
}
static inline void xlat_cmp(Nes *n, uint8_t r, uint8_t m) {
    XP = (uint8_t)((XP & ~0x01) | (r >= m ? 1 : 0));
    XZN((uint8_t)(r - m));
}
static inline uint8_t xlat_asl(Nes *n, uint8_t v) { XP = (uint8_t)((XP & ~1) | (v >> 7)); v <<= 1; XZN(v); return v; }
static inline uint8_t xlat_lsr(Nes *n, uint8_t v) { XP = (uint8_t)((XP & ~1) | (v & 1)); v >>= 1; XZN(v); return v; }
static inline uint8_t xlat_rol(Nes *n, uint8_t v) { uint8_t r = (uint8_t)((v << 1) | (XP & 1)); XP = (uint8_t)((XP & ~1) | (v >> 7)); XZN(r); return r; }
static inline uint8_t xlat_ror(Nes *n, uint8_t v) { uint8_t r = (uint8_t)((v >> 1) | ((XP & 1) << 7)); XP = (uint8_t)((XP & ~1) | (v & 1)); XZN(r); return r; }
static inline void xlat_bit(Nes *n, uint8_t m) {
    XP = (uint8_t)((XP & ~0xC2) | (m & 0xC0) | ((XA & m) ? 0 : 0x02));
}

/* Регистрация всех переведённых процедур (сгенерированный файл). */
void xlat_register_all(Nes *n);
/* Список переведённых процедур: для отчётов и проверки. */
typedef struct XlatInfo {
    int bank;            /* банк для $8000-$BFFF, иначе -1 */
    uint16_t entry;      /* адрес входа */
    int kind;            /* 0 = вызывается по JSR, 1 = обработчик прерывания (RTI),
                          * 2 = круг бесконечного цикла (вход — начало круга, выход — возврат к нему) */
    int bytes;           /* сколько байт кода покрывает перевод */
    nes_hook_fn fn;
    const char *name;
} XlatInfo;
extern const XlatInfo xlat_table[];
extern const int xlat_count;

#endif
