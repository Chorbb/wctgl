/* tgl_api.h — интерфейс ядра для Godot (C#) и других программ.
 * Простые функции с C-соглашением вызова: их легко вызывать через P/Invoke. */
#ifndef TGL_API_H
#define TGL_API_H

#include <stdint.h>

#if defined(_WIN32)
#  define TGL_API __declspec(dllexport)
#else
#  define TGL_API __attribute__((visibility("default")))
#endif

/* Версия интерфейса: меняется, если меняются функции ниже. 2 — добавлен звук. */
TGL_API int tgl_api_version(void);
/* Загрузить ROM (копируется внутрь) и перезагрузить игру. 0 = успех, <0 = ошибка формата. */
TGL_API int tgl_load_rom(const uint8_t *rom, int size);
/* Кнопка Reset. */
TGL_API void tgl_reset(void);
/* Один кадр игры с заданными кнопками: A B Select Start Up Down Left Right = биты 0..7. */
TGL_API void tgl_run_frame(int buttons);
/* Последний кадр картинки: 256 x 240 пикселей, по 4 байта (R, G, B, A). */
TGL_API void tgl_get_frame_rgba(uint8_t *out);
/* Сколько кадров прошло с перезагрузки. */
TGL_API int tgl_frame_count(void);
/* Чтение оперативной памяти игры в момент конца последнего кадра (для отладки и инструментов мода). */
TGL_API int tgl_peek(int addr);

/* Переведённый код: 1 = включён (по умолчанию), 0 = всё исполняет эмулятор оригинала.
 * Игра в обоих режимах идёт одинаково до такта — это проверяет tests/xtest.c. */
TGL_API void tgl_use_translation(int on);
/* Сколько процедур игры переведено в этой сборке ядра. */
TGL_API int tgl_translated_routines(void);

/* ---- звук (версия 2) ----
 * Частота вывода (Гц). Godot подстраивает её на доли процента, чтобы буфер звука
 * не переполнялся и не пустел (часы звука и кадров немного расходятся). */
TGL_API void tgl_audio_set_rate(double hz);
/* Забрать накопленный звук: до max отсчётов, 16 бит, моно. Возвращает сколько забрано. */
TGL_API int tgl_audio_read(int16_t *out, int max);

#endif
