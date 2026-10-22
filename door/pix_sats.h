/*
 * pix_sats.h -- the satellite tracker as JPEG XL pictures, for terminals that speak the CTerm APC picture commands
 * but not TRACE. See pix_sats.c.
 */
#ifndef PIX_SATS_H
#define PIX_SATS_H

#include <stdbool.h>

#include "sats.h"

typedef struct {
    bool blob;          /* pictures can travel inside the command (CTerm 1.329+), not only through the cache */
    int  px_w, px_h;    /* the screen in pixels */
} pix_caps_t;

/* Runs the tracker until the caller leaves. False if the caller hung up. */
bool pix_sats_run(const pix_caps_t *caps, SatUserPrefs *prefs);

#endif
