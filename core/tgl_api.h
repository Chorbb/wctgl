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

/* Версия интерфейса: меняется, если меняются функции ниже. */
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
/* Чтение оперативной памяти игры (для отладки и будущих инструментов мода). */
TGL_API int tgl_peek(int addr);

#endif
