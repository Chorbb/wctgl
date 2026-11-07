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

/* Версия интерфейса: меняется, если меняются функции ниже. 2 — звук, 3 — правка ROM (моды), 4 — сохранения. */
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

/* ---- моды (версия 3) ----
 * Правка копии ROM в памяти: банк 0..7, адрес процессора, байты. 0 = успех.
 * После правок вызовите tgl_reset(), чтобы игра началась с ними. */
TGL_API int tgl_patch_rom(int bank, int addr, const uint8_t *data, int len);

/* ---- сохранения (версия 4) ----
 * Размер снимка; сохранить в буфер (возвращает размер или <0); загрузить (0 = успех,
 * -2 не сохранение, -3 другая сборка ядра, -4 другой ROM или моды). Вызывать между кадрами. */
TGL_API int tgl_state_size(void);
TGL_API int tgl_state_save(uint8_t *buf, int cap);
TGL_API int tgl_state_load(const uint8_t *buf, int len);

/* Описание сборки ядра (UTF-8): дата сборки, число переведённых процедур и новых врагов. */
TGL_API int tgl_build_info(char *buf, int cap);

#endif
