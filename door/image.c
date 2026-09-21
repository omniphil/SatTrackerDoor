/*
 * image.c - shrinks a story's picture to a small JPEG for the wire. See image.h.
 *
 * A news picture is often a megabyte; the TRACE module gets a 256x144 JPEG of
 * about 10 KB instead, and the ANSI view the same pixels as half blocks. Both
 * stb libraries are public domain (third_party/).
 */
#include "image.h"

#include <stdlib.h>
#include <string.h>

#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_ONLY_GIF
#define STBI_ONLY_BMP
#define STBI_NO_STDIO
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#include "third_party/stb_image.h"

#define STBI_WRITE_NO_STDIO
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "third_party/stb_image_write.h"

#define JPEG_QUALITY 80

typedef struct { unsigned char *buf; size_t len, cap; } Sink;

static void sink(void *ctx, void *data, int size)
{
    Sink *s = ctx;
    if (s->len + (size_t)size > s->cap) {
        size_t cap = s->cap ? s->cap * 2 : 32768;
        while (cap < s->len + (size_t)size) cap *= 2;
        unsigned char *nb = realloc(s->buf, cap);
        if (!nb) return;
        s->buf = nb;
        s->cap = cap;
    }
    memcpy(s->buf + s->len, data, (size_t)size);
    s->len += (size_t)size;
}

unsigned char *image_thumbnail(const unsigned char *raw, size_t len, int w, int h, size_t *outLen)
{
    int sw, sh, n;
    unsigned char *src = stbi_load_from_memory(raw, (int)len, &sw, &sh, &n, 3);
    if (!src) return NULL;
    if (sw < 48 || sh < 32) { stbi_image_free(src); return NULL; }   /* a tracking pixel or an icon */

    /* Crop to the target's shape, keeping the middle. */
    double want = (double)w / h, have = (double)sw / sh;
    int cx = 0, cy = 0, cw = sw, ch = sh;
    if (have > want) { cw = (int)(sh * want); cx = (sw - cw) / 2; }
    else             { ch = (int)(sw / want); cy = (sh - ch) / 3; }   /* faces sit high: favour the top */

    /* Area-average down (or nearest up, for small pictures). */
    unsigned char *dst = malloc((size_t)w * h * 3);
    if (!dst) { stbi_image_free(src); return NULL; }
    for (int y = 0; y < h; y++) {
        int y0 = cy + y * ch / h, y1 = cy + (y + 1) * ch / h;
        if (y1 <= y0) y1 = y0 + 1;
        for (int x = 0; x < w; x++) {
            int x0 = cx + x * cw / w, x1 = cx + (x + 1) * cw / w;
            if (x1 <= x0) x1 = x0 + 1;
            unsigned r = 0, g = 0, b = 0, count = 0;
            for (int yy = y0; yy < y1 && yy < sh; yy++)
                for (int xx = x0; xx < x1 && xx < sw; xx++) {
                    const unsigned char *px = src + ((size_t)yy * sw + xx) * 3;
                    r += px[0]; g += px[1]; b += px[2]; count++;
                }
            if (!count) count = 1;
            unsigned char *o = dst + ((size_t)y * w + x) * 3;
            o[0] = (unsigned char)(r / count);
            o[1] = (unsigned char)(g / count);
            o[2] = (unsigned char)(b / count);
        }
    }
    stbi_image_free(src);

    Sink s = { 0 };
    int ok = stbi_write_jpg_to_func(sink, &s, w, h, 3, dst, JPEG_QUALITY);
    free(dst);
    if (!ok || !s.len) { free(s.buf); return NULL; }
    *outLen = s.len;
    return s.buf;
}

unsigned char *image_decode_rgb(const unsigned char *jpg, size_t len, int w, int h)
{
    int sw, sh, n;
    unsigned char *px = stbi_load_from_memory(jpg, (int)len, &sw, &sh, &n, 3);
    if (!px) return NULL;
    if (sw != w || sh != h) { stbi_image_free(px); return NULL; }
    return px;
}

unsigned char *image_decode_any(const unsigned char *buf, size_t len, int *w, int *h, int channels)
{
    int n;
    return stbi_load_from_memory(buf, (int)len, w, h, &n, channels);
}
