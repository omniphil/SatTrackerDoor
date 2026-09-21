/*
 * image.h - shrinks a story's picture to a small JPEG for the wire.
 */
#ifndef NEWS_DOOR_IMAGE_H
#define NEWS_DOOR_IMAGE_H

#include <stddef.h>

/* Decodes raw (JPEG, PNG, GIF or BMP), crops it to fill w x h, and re-encodes
 * it as a JPEG. Malloc'd; NULL when it can't be read or is too small. */
unsigned char *image_thumbnail(const unsigned char *raw, size_t len, int w, int h, size_t *outLen);

/* Decodes a JPEG made by image_thumbnail back to w*h RGB pixels (malloc'd). */
unsigned char *image_decode_rgb(const unsigned char *jpg, size_t len, int w, int h);

/* Decodes any picture stb can read (JPEG, PNG, GIF, BMP) to w*h pixels of
 * `channels` bytes each (malloc'd), or NULL. */
unsigned char *image_decode_any(const unsigned char *buf, size_t len, int *w, int *h, int channels);

#endif
