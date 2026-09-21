// Prints the orbit code's positions for every satellite in a TLE file at a few
// times, for tools/check_orbit.py to compare with the reference sgp4 library.
//   cc -O2 -I ../module/src orbit_dump.c -lm -o orbit_dump && ./orbit_dump file.tle
#include <stdio.h>
#include <string.h>
#include "orbit.h"

int main(int argc, char **argv)
{
    FILE *f = fopen(argv[1], "r");
    char a[128], b[128], c[128];
    while (fgets(a, sizeof a, f) && fgets(b, sizeof b, f) && fgets(c, sizeof c, f)) {
        OrbElements e;
        if (!orb_parse_tle(a, b, c, &e)) continue;
        OrbSat s;
        orb_init(&s, &e);
        for (int k = 0; k < 4; k++) {
            double t = e.epoch + k * 43200.0;          // epoch, +12h, +24h, +36h
            OrbVec r;
            int ok = orb_propagate(&s, t, &r);
            printf("%u %d %.3f %d %.6f %.6f %.6f\n", e.norad, s.deep, t, ok, r.x, r.y, r.z);
        }
    }
    return 0;
}
