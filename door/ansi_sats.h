/*
 * ansi_sats.h - the Satellite Tracker as a 24-bit ANSI screen, for callers who
 * pick ANSI (or have no TRACE).
 */
#ifndef ANSI_SATS_H
#define ANSI_SATS_H

#include "sats.h"

/* Runs until the caller quits; prefs are saved as they change. */
void ansi_sats_run(SatUserPrefs *prefs);

#endif
