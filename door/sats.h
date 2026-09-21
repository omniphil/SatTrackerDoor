/*
 * sats.h - the Satellite Tracker door's data: config, each caller's home place
 * and choices, place search (Open-Meteo), and orbit elements (CelesTrak),
 * all cached beside the binary.
 */
#ifndef SATS_H
#define SATS_H

#include <stdbool.h>
#include <stddef.h>

#include "orbit.h"
#include "protocol.h"

typedef struct {
    char   place[SAT_NAME_LEN];      /* where callers start before they pick */
    double lat, lon;
    int    cacheHours;               /* orbit elements are reused this long (min 2, CelesTrak's rule) */
    char   weatherSaves[512];        /* the Weather door's saves folder, to borrow a caller's place */
} SatConfig;

typedef struct {
    bool     havePlace;
    char     place[SAT_NAME_LEN];
    double   lat, lon;
    int      utcOffset;              /* seconds east of UTC at the place */
    int      group;
    uint32_t selected;               /* NORAD number */
    int      display;                /* start page: 1 TRACE, 2 ANSI, 0 none */
} SatUserPrefs;

extern SatConfig sat_config;

const char *sat_beside_exe(const char *name, char *out, size_t size);
void sat_load_config(void);
void sat_load_prefs(SatUserPrefs *p);
void sat_save_prefs(const SatUserPrefs *p);

/* Looks a place up by name, "City, ST" or postcode. Count found, or -1 when the
 * lookup failed. */
int sat_search(const char *query, SatPlace *out, int max);

/* The UTC offset (seconds) at a place right now, from Open-Meteo; false if it
 * couldn't be reached (the stored one stands). */
bool sat_utc_offset(double lat, double lon, int *offset);

/* A group's satellites (CelesTrak, cached). Count, or -1 with a message. */
int sat_group(int group, OrbElements *out, int max, char *err, size_t errSize);

#endif
