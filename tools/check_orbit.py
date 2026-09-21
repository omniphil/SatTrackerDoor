#!/usr/bin/env python3
"""Checks orbit.h against the reference sgp4 library (pip install sgp4).

    python3 check_orbit.py <tle file> <orbit_dump output>
Prints the worst position difference for near-Earth (SGP4) and deep-space
(the Kepler + J2 stand-in) satellites.
"""
import sys
from sgp4.api import Satrec, jday
import datetime

tle = open(sys.argv[1]).read().splitlines()
sats = {}
for i in range(0, len(tle) - 2, 3):
    s = Satrec.twoline2rv(tle[i + 1], tle[i + 2])
    sats[s.satnum] = (tle[i].strip(), s)

worst = {0: (0, ""), 1: (0, "")}
for line in open(sys.argv[2]):
    norad, deep, t, ok, x, y, z = line.split()
    norad, deep, t, ok = int(norad), int(deep), float(t), int(ok)
    if norad not in sats or not ok:
        continue
    name, s = sats[norad]
    d = datetime.datetime.fromtimestamp(t, datetime.timezone.utc)
    jd, fr = jday(d.year, d.month, d.day, d.hour, d.minute, d.second + d.microsecond / 1e6)
    e, r, v = s.sgp4(jd, fr)
    if e:
        continue
    err = ((r[0] - float(x)) ** 2 + (r[1] - float(y)) ** 2 + (r[2] - float(z)) ** 2) ** 0.5
    if err > worst[deep][0]:
        worst[deep] = (err, name)
print(f"near-Earth (SGP4): worst {worst[0][0]:.4f} km ({worst[0][1]})")
print(f"deep space (Kepler+J2): worst {worst[1][0]:.1f} km ({worst[1][1]})")
