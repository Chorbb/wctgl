/* rt.c — реализации memcpy/memset для сборки без среды выполнения C (tglcore.dll). */
#ifdef TGL_FREESTANDING
#include "rt.h"

void *memcpy(void *dst, const void *src, size_t n) {
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    while (n--) *d++ = *s++;
    return dst;
}

void *memset(void *dst, int c, size_t n) {
    unsigned char *d = (unsigned char *)dst;
    while (n--) *d++ = (unsigned char)c;
    return dst;
}
#endif

#ifdef TGL_FREESTANDING
/* компилятор для Windows ждёт этот символ, если в программе есть дробные числа */
int _fltused = 0;
#endif
