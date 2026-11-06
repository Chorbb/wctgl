/* rt.h — минимум стандартной библиотеки, нужный ядру.
 * На Linux/macOS берётся из libc; в сборке библиотеки для Windows (TGL_FREESTANDING)
 * реализации лежат в rt.c — так tglcore.dll не зависит от среды выполнения C. */
#ifndef TGL_RT_H
#define TGL_RT_H

#include <stddef.h>

void *memcpy(void *dst, const void *src, size_t n);
void *memset(void *dst, int c, size_t n);

#endif
