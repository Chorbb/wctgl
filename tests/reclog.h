/* reclog.h — чтение записи из Mesen (TGLLOG3/4): кнопки по кадрам и память в момент перезагрузки.
 * Нужна тестам, которым запись служит только источником нажатий. */
#ifndef TGL_RECLOG_H
#define TGL_RECLOG_H
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct RecLog {
    int frames;          /* сколько кадров после перезагрузки */
    uint8_t *input;      /* кнопки кадра k (k = 0 — первый кадр после перезагрузки) */
    uint8_t ram0[2048];  /* память перед перезагрузкой */
} RecLog;

static uint8_t *rl_read_file(const char *path, long *size) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); *size = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *d = malloc((size_t)*size);
    if (fread(d, 1, (size_t)*size, f) != (size_t)*size) { fclose(f); free(d); return NULL; }
    fclose(f);
    return d;
}

static uint32_t rl_u32(const uint8_t *p) { return (uint32_t)(p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24)); }

/* 0 при успехе */
static int reclog_load(RecLog *r, const char *path) {
    long size;
    uint8_t *log = rl_read_file(path, &size);
    if (!log) return -1;
    int extra = 0;
    long i = 8;
    if (memcmp(log, "TGLLOG4\n", 8) == 0) {
        long e = 8; while (e < size && log[e] != '\n') e++;
        extra = 1;
        for (long k = 8; k < e; k++) if (log[k] == ',') extra++;
        i = e + 1;
    } else if (memcmp(log, "TGLLOG3\n", 8) != 0) { free(log); return -2; }
    int rec_fixed = 7 + 12 + 4 * extra + 2048 + 256;
    int cap = 1 << 15, n = 0;
    long *pos = malloc(sizeof(long) * (size_t)cap);
    while (i + rec_fixed <= size && log[i] == 'F') {
        if (n == cap) { cap *= 2; pos = realloc(pos, sizeof(long) * (size_t)cap); }
        pos[n++] = i;
        i += rec_fixed + ((log[i + 6] & 1) ? 2048 : 0);
    }
    int start = -1;
    for (int k = 1; k < n; k++)
        if (rl_u32(log + pos[k] + 7 + 8) < rl_u32(log + pos[k - 1] + 7 + 8)) { start = k - 1; break; }
    if (start < 0) { free(pos); free(log); return -3; }
    memcpy(r->ram0, log + pos[start] + 7 + 12 + 4 * extra, 2048);
    r->frames = n - start - 1;
    r->input = malloc((size_t)r->frames + 1);
    for (int k = 0; k < r->frames; k++) r->input[k] = log[pos[start + 1 + k] + 5];
    free(pos); free(log);
    return 0;
}
#endif
