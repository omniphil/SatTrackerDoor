/*
 * ansi_sats.c - the Satellite Tracker as a 24-bit ANSI screen. See ansi_sats.h.
 *
 * A live world map in half blocks (64x32 pixels: the day side in NASA's Blue
 * Marble colours, the night side dark with its city lights), the satellites
 * moving across it every couple of seconds, the chosen one's path, a list to
 * pick from, and its passes over the caller's town. Laid out in 79x24 so nothing
 * lands in the screen's last column or row, and sent as a diff so a frame costs
 * only the cells that changed.
 */
#include "ansi_sats.h"

#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "door.h"
#include "image.h"

#define ROWS 24
#define COLS 79
#define CSI "\033["

#define C_BG      0x070B18
#define C_BAR     0x0D1428
#define C_PANEL   0x111933
#define C_PANEL_HI 0x1E2A52
#define C_EDGE    0x2A3862
#define C_TEXT    0xEAF0FA
#define C_TEXT2   0xA3AECB
#define C_TEXT3   0x66739A
#define C_ACCENT  0x4FD1FF
#define C_MAGENTA 0xFF5FD2
#define C_SUN     0xFFC53D
#define C_WARN    0xFFB547
#define C_ERR     0xFF6B6B
#define C_GOOD    0x6EDC8C
#define C_SAT     0xFFE45C
#define C_SEL     0xFFFFFF
#define C_TRACK   0x4FD1FF
#define C_HOME    0xFF4F6B

typedef struct { char ch; uint32_t fg, bg; } Cell;

static Cell g_scr[ROWS][COLS];
static Cell g_sent[ROWS][COLS];
static bool g_sentValid = false;

static uint32_t mix(uint32_t a, uint32_t b, float t)
{
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    int ar = (a >> 16) & 255, ag = (a >> 8) & 255, ab = a & 255;
    int br = (b >> 16) & 255, bg = (b >> 8) & 255, bb = b & 255;
    return (uint32_t)((int)(ar + (br - ar) * t) << 16 | (int)(ag + (bg - ag) * t) << 8 | (int)(ab + (bb - ab) * t));
}

static void clear(uint32_t bg)
{
    for (int r = 0; r < ROWS; r++)
        for (int c = 0; c < COLS; c++) g_scr[r][c] = (Cell){ ' ', C_TEXT, bg };
}

static void fill(int r, int c, int h, int w, uint32_t bg)
{
    for (int y = r; y < r + h && y < ROWS; y++)
        for (int x = c; x < c + w && x < COLS; x++)
            if (y >= 0 && x >= 0) g_scr[y][x] = (Cell){ ' ', C_TEXT, bg };
}

/* Writes text at (r, c), 0-based; bg 0xFFFFFFFF keeps what's there. Returns the column after it. */
#define KEEP 0xFFFFFFFFu
static int put(int r, int c, const char *s, uint32_t fg, uint32_t bg)
{
    if (r < 0 || r >= ROWS) return c;
    for (; *s; s++, c++) {
        if (c < 0) continue;
        if (c >= COLS) break;
        unsigned char ch = (unsigned char)*s;
        g_scr[r][c].ch = ch < 0x20 ? ' ' : (char)ch;
        g_scr[r][c].fg = fg;
        if (bg != KEEP) g_scr[r][c].bg = bg;
    }
    return c;
}

static int putf(int r, int c, uint32_t fg, uint32_t bg, const char *fmt, ...)
{
    char buf[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    return put(r, c, buf, fg, bg);
}

static void put_c(int r, int c0, int w, const char *s, uint32_t fg, uint32_t bg)
{
    int len = (int)strlen(s);
    put(r, c0 + (w - len) / 2, s, fg, bg);
}

static void put_r(int r, int cEnd, const char *s, uint32_t fg, uint32_t bg)
{
    put(r, cEnd - (int)strlen(s), s, fg, bg);
}

static void setch(int r, int c, char ch, uint32_t fg, uint32_t bg)
{
    if (r < 0 || r >= ROWS || c < 0 || c >= COLS) return;
    g_scr[r][c].ch = ch;
    g_scr[r][c].fg = fg;
    if (bg != KEEP) g_scr[r][c].bg = bg;
}

/* Sends what changed since the last flush. */
static void flush(void)
{
    static char out[96 * 1024];
    size_t n = 0;
    uint32_t fg = KEEP, bg = KEEP;
    int cr = -1, cc = -1;
#define EMIT(...) (n += (size_t)snprintf(out + n, sizeof out - n, __VA_ARGS__))
    for (int r = 0; r < ROWS; r++)
        for (int c = 0; c < COLS; c++) {
            Cell *want = &g_scr[r][c];
            if (g_sentValid && !memcmp(want, &g_sent[r][c], sizeof *want)) continue;
            if (n == 0) EMIT(CSI "?25l");
            if (cr != r || cc != c) EMIT(CSI "%d;%dH", r + 1, c + 1);
            if (want->bg != bg) { EMIT(CSI "48;2;%u;%u;%um", (want->bg >> 16) & 255, (want->bg >> 8) & 255, want->bg & 255); bg = want->bg; }
            if (want->fg != fg && want->ch != ' ') { EMIT(CSI "38;2;%u;%u;%um", (want->fg >> 16) & 255, (want->fg >> 8) & 255, want->fg & 255); fg = want->fg; }
            out[n++] = want->ch;
            g_sent[r][c] = *want;
            cr = r;
            cc = c + 1;
            if (n > sizeof out - 256) { door_write_raw(out, n); n = 0; }
        }
    g_sentValid = true;
    if (n) door_write_raw(out, n);
#undef EMIT
}


static void invalidate(void) { g_sentValid = false; }

/* ------------------------------------------------------------- state --- */

#define MAP_W 64
#define MAP_H 32                    /* pixels; 16 text rows of half blocks */
#define MAP_R 1
#define LIST_C 65
#define LIST_W 14

static SatUserPrefs *g_prefs;
static OrbSat  g_sats[SAT_MAX];
static int     g_n = 0, g_sel = 0, g_top = 0;
static OrbPass g_pass[4];
static int     g_nPass = 0;
static time_t  g_passAt = 0;
static uint32_t g_day[MAP_W * MAP_H];     /* the Earth's day colours, map-sized */
static uint8_t  g_night[MAP_W * MAP_H];   /* city lights */
static bool     g_haveEarth = false;
static char     g_status[210];
static uint32_t g_statusCol = C_TEXT2;
static time_t   g_statusUntil = 0;
static char     g_msg[1];

static void status(const char *s, uint32_t col, int secs)
{
    snprintf(g_status, sizeof g_status, "%s", s);
    g_statusCol = col;
    g_statusUntil = time(NULL) + secs;
}

/* The Earth pictures (earth.bin), shrunk to the map. */
static void load_earth(void)
{
    char path[1100];
    FILE *f = fopen(sat_beside_exe("earth.bin", path, sizeof path), "rb");
    if (!f) return;
    unsigned char head[12];
    if (fread(head, 1, 12, f) != 12 || memcmp(head, "EAR1", 4)) { fclose(f); return; }
    uint32_t dl, nl;
    memcpy(&dl, head + 4, 4);
    memcpy(&nl, head + 8, 4);
    unsigned char *d = malloc(dl), *n = malloc(nl);
    int ok = d && n && fread(d, 1, dl, f) == dl && fread(n, 1, nl, f) == nl;
    fclose(f);
    int w, h, w2, h2;
    unsigned char *day = ok ? image_decode_any(d, dl, &w, &h, 3) : NULL;
    unsigned char *night = ok ? image_decode_any(n, nl, &w2, &h2, 1) : NULL;
    free(d);
    free(n);
    if (day && night) {
        for (int y = 0; y < MAP_H; y++)
            for (int x = 0; x < MAP_W; x++) {
                int x0 = x * w / MAP_W, x1 = (x + 1) * w / MAP_W, y0 = y * h / MAP_H, y1 = (y + 1) * h / MAP_H;
                unsigned r = 0, g = 0, b = 0, c = 0, lights = 0;
                for (int yy = y0; yy < y1; yy++)
                    for (int xx = x0; xx < x1; xx++) {
                        const unsigned char *p = day + (yy * w + xx) * 3;
                        r += p[0]; g += p[1]; b += p[2]; c++;
                        int nx = xx * w2 / w, ny = yy * h2 / h;
                        lights += night[ny * w2 + nx];
                    }
                if (!c) c = 1;
                g_day[y * MAP_W + x] = (r / c) << 16 | (g / c) << 8 | (b / c);
                g_night[y * MAP_W + x] = (uint8_t)(lights / c);
            }
        g_haveEarth = true;
    }
    free(day);
    free(night);
}

static void load_group(int group)
{
    static OrbElements e[SAT_MAX];
    char err[200];
    status("Fetching orbits...", C_WARN, 30);
    int n = sat_group(group, e, SAT_MAX, err, sizeof err);
    if (n < 0) { status(err, C_ERR, 10); n = 0; }
    g_n = n;
    g_sel = 0;
    for (int i = 0; i < n; i++) {
        orb_init(&g_sats[i], &e[i]);
        if (e[i].norad == g_prefs->selected) g_sel = i;
    }
    g_top = g_sel > LIST_W ? g_sel - 4 : 0;
    g_passAt = 0;
    if (n) { char m[80]; snprintf(m, sizeof m, "%d satellites", n); status(m, C_GOOD, 3); }
}

static void refresh_passes(void)
{
    g_nPass = 0;
    if (!g_n) return;
    g_nPass = orb_passes(&g_sats[g_sel], g_prefs->lat * ORB_DEG, g_prefs->lon * ORB_DEG, 0.05,
                         (double)time(NULL), 72, 10, g_pass, 4);
    g_passAt = time(NULL);
}

/* ----------------------------------------------------------- drawing --- */

static void fmt_local(char *out, size_t n, double t, const char *fmt)
{
    time_t lt = (time_t)(t + (g_prefs->utcOffset > -900000 ? g_prefs->utcOffset : 0));
    struct tm tm;
    gmtime_r(&lt, &tm);
    strftime(out, n, fmt, &tm);
}

static void draw_header(void)
{
    fill(0, 0, 1, COLS, C_BAR);
    int c = put(0, 1, "TERM", C_MAGENTA, C_BAR);
    c = put(0, c, "inator", C_ACCENT, C_BAR);
    c = put(0, c + 1, "Satellite Tracker", C_TEXT, C_BAR);
    put(0, c + 1, "\xB3", C_EDGE, C_BAR);
    char tm[32], utc[16];
    fmt_local(tm, sizeof tm, (double)time(NULL), "%a %H:%M:%S");
    time_t now = time(NULL);
    struct tm u;
    gmtime_r(&now, &u);
    strftime(utc, sizeof utc, "%H:%MZ", &u);
    put_r(0, COLS - 1, utc, C_TEXT3, C_BAR);
    put_r(0, COLS - 2 - (int)strlen(utc), tm, C_TEXT2, C_BAR);
    /* The place gets what room is left between the title and the clocks. */
    int room = COLS - 3 - (int)strlen(utc) - (int)strlen(tm) - (c + 3);
    char place[64];
    if (room > 3) {
        if ((int)strlen(g_prefs->place) > room) snprintf(place, sizeof place, "%.*s..", room - 2, g_prefs->place);
        else snprintf(place, sizeof place, "%s", g_prefs->place);
        put(0, c + 3, place, C_TEXT, C_BAR);
    }
}

/* A satellite's map pixel. */
static int map_px(double lat, double lon, int *x, int *y)
{
    double fx = (lon / ORB_DEG + 180.0) / 360.0 * MAP_W, fy = (90.0 - lat / ORB_DEG) / 180.0 * MAP_H;
    *x = (int)fx;
    *y = (int)fy;
    if (*x >= MAP_W) *x -= MAP_W;
    if (*x < 0) *x += MAP_W;
    return *y >= 0 && *y < MAP_H;
}

static void draw_map(void)
{
    static uint32_t px[MAP_W * MAP_H];
    double now = (double)time(NULL);
    double slat, slon;
    orb_subsolar(now, &slat, &slon);
    double ss = sin(slat), cs = cos(slat);
    for (int y = 0; y < MAP_H; y++) {
        double lat = (90.0 - (y + 0.5) * 180.0 / MAP_H) * ORB_DEG;
        double sl = sin(lat), cl = cos(lat);
        for (int x = 0; x < MAP_W; x++) {
            double lon = ((x + 0.5) * 360.0 / MAP_W - 180.0) * ORB_DEG;
            double cz = sl * ss + cl * cs * cos(lon - slon);          /* sun's height: + day, - night */
            float day = (float)((cz + 0.08) / 0.16);
            uint32_t d = g_haveEarth ? g_day[y * MAP_W + x] : ((x + y) % 7 ? 0x1A3A6A : 0x2E5A3A);
            uint32_t nightCol = mix(mix(d, 0x000000, 0.82f), 0xFFC66A, g_haveEarth ? g_night[y * MAP_W + x] / 255.0f * 0.95f : 0);
            px[y * MAP_W + x] = mix(nightCol, d, day);
        }
    }
    /* The chosen satellite's path for the next orbit, dotted. */
    if (g_n) {
        OrbSat *s = &g_sats[g_sel];
        double period = 1440.0 / s->e.nRevDay * 60.0;
        for (double t = now; t < now + period; t += period / 90.0) {
            double la, lo, al;
            int x, y;
            if (orb_where(s, t, &la, &lo, &al, NULL) && map_px(la, lo, &x, &y) && ((int)((t - now) / (period / 90.0)) % 2 == 0))
                px[y * MAP_W + x] = mix(px[y * MAP_W + x], C_TRACK, 0.85f);
        }
    }
    /* Home. */
    int hx, hy;
    if (map_px(g_prefs->lat * ORB_DEG, g_prefs->lon * ORB_DEG, &hx, &hy)) px[hy * MAP_W + hx] = C_HOME;
    /* Every satellite in the group, then the chosen one on top. */
    for (int i = 0; i < g_n; i++) {
        double la, lo, al;
        int x, y;
        if (i != g_sel && orb_where(&g_sats[i], now, &la, &lo, &al, NULL) && map_px(la, lo, &x, &y))
            px[y * MAP_W + x] = C_SAT;
    }
    if (g_n) {
        double la, lo, al;
        int x, y;
        if (orb_where(&g_sats[g_sel], now, &la, &lo, &al, NULL) && map_px(la, lo, &x, &y)) px[y * MAP_W + x] = C_SEL;
    }
    for (int r = 0; r < MAP_H / 2; r++)
        for (int x = 0; x < MAP_W; x++)
            setch(MAP_R + r, x, (char)0xDF, px[(r * 2) * MAP_W + x], px[(r * 2 + 1) * MAP_W + x]);
}

static void draw_list(void)
{
    fill(MAP_R, LIST_C - 1, MAP_H / 2, LIST_W + 1, C_PANEL);
    put(MAP_R, LIST_C, SAT_GROUP_NAME[g_prefs->group], C_ACCENT, C_PANEL);
    int rows = MAP_H / 2 - 2;
    if (g_sel < g_top) g_top = g_sel;
    if (g_sel >= g_top + rows) g_top = g_sel - rows + 1;
    for (int k = 0; k < rows && g_top + k < g_n; k++) {
        int i = g_top + k;
        bool sel = i == g_sel;
        char nm[16];
        snprintf(nm, sizeof nm, "%-13.13s", g_sats[i].e.name);
        if (sel) fill(MAP_R + 1 + k, LIST_C - 1, 1, LIST_W + 1, C_PANEL_HI);
        put(MAP_R + 1 + k, LIST_C - 1, sel ? "\xAF" : " ", C_ACCENT, sel ? C_PANEL_HI : C_PANEL);
        put(MAP_R + 1 + k, LIST_C, nm, sel ? C_TEXT : C_TEXT2, sel ? C_PANEL_HI : C_PANEL);
    }
    char pos[20];
    snprintf(pos, sizeof pos, "%d/%d", g_n ? g_sel + 1 : 0, g_n);
    put_r(MAP_R + MAP_H / 2 - 1, COLS - 1, pos, C_TEXT3, C_PANEL);
}

static void draw_info(void)
{
    const int r0 = MAP_R + MAP_H / 2;
    fill(r0, 0, 6, COLS, C_BG);
    if (!g_n) { put(r0 + 1, 2, "No satellites loaded.", C_TEXT2, C_BG); return; }
    OrbSat *s = &g_sats[g_sel];
    double now = (double)time(NULL), la, lo, al;
    OrbVec r;
    char buf[96];
    put(r0, 1, s->e.name, C_TEXT, C_BG);
    if (orb_where(s, now, &la, &lo, &al, &r)) {
        OrbVec r2;
        double la2, lo2, al2, v = 0;
        if (orb_where(s, now + 1, &la2, &lo2, &al2, &r2))
            v = sqrt((r2.x - r.x) * (r2.x - r.x) + (r2.y - r.y) * (r2.y - r.y) + (r2.z - r.z) * (r2.z - r.z));
        snprintf(buf, sizeof buf, "Altitude  %.0f km (%.0f mi)", al, al * 0.621371);
        put(r0 + 1, 1, buf, C_TEXT2, C_BG);
        snprintf(buf, sizeof buf, "Speed     %.2f km/s (%.0f mph)", v, v * 2236.94);
        put(r0 + 2, 1, buf, C_TEXT2, C_BG);
        snprintf(buf, sizeof buf, "Over      %.1f%c %.1f%c", fabs(la / ORB_DEG), la >= 0 ? 'N' : 'S',
                 fabs(lo / ORB_DEG), lo >= 0 ? 'E' : 'W');
        put(r0 + 3, 1, buf, C_TEXT2, C_BG);
        bool lit = orb_sunlit(r, orb_sun(now));
        put(r0 + 4, 1, lit ? "In sunlight" : "In Earth's shadow", lit ? C_SUN : C_TEXT3, C_BG);
    }
    snprintf(buf, sizeof buf, "Orbit %.0f min, %.1f\xF8 incl.", 1440.0 / s->e.nRevDay, s->e.incl / ORB_DEG);
    put(r0 + 5, 1, buf, C_TEXT3, C_BG);

    /* Passes over home. */
    const int c0 = 38;
    snprintf(buf, sizeof buf, "Passes over %.26s", g_prefs->place);
    put(r0, c0, buf, C_ACCENT, C_BG);
    if (s->deep) {
        put(r0 + 2, c0, "A high orbit: it doesn't pass over,", C_TEXT3, C_BG);
        put(r0 + 3, c0, "it hangs in the sky or drifts slowly.", C_TEXT3, C_BG);
        return;
    }
    if (!g_nPass) { put(r0 + 2, c0, "None above 10\xF8 in the next 3 days.", C_TEXT3, C_BG); return; }
    for (int i = 0; i < g_nPass; i++) {
        const OrbPass *p = &g_pass[i];
        char when[32];
        fmt_local(when, sizeof when, p->aos, "%a %d %b %H:%M");
        char dir[16];
        snprintf(dir, sizeof dir, "%s-%s", orb_compass(p->azAos), orb_compass(p->azLos));
        snprintf(buf, sizeof buf, "%s  %2.0f\xF8 %-7s", when, p->maxEl / ORB_DEG, dir);
        int c = put(r0 + 1 + i, c0, buf, C_TEXT, C_BG);
        if (p->visible) put(r0 + 1 + i, c + 1, "VISIBLE", 0x000000, C_GOOD);
    }
}

static void draw_keys(void)
{
    fill(ROWS - 2, 0, 1, COLS, C_BG);
    int c = 1;
    for (int g = 0; g < GRP_COUNT; g++) {
        char k[4];
        snprintf(k, sizeof k, "%d", g + 1);
        bool on = g == g_prefs->group;
        c = put(ROWS - 2, c, " ", C_TEXT, on ? C_ACCENT : C_PANEL_HI);
        c = put(ROWS - 2, c, k, on ? 0x000000 : C_TEXT, on ? C_ACCENT : C_PANEL_HI);
        c = put(ROWS - 2, c, " ", C_TEXT, on ? C_ACCENT : C_PANEL_HI);
        static const char *const SHORT[GRP_COUNT] = { "Stations", "Brightest", "Weather", "Ham radio", "GPS", "Science" };
        c = put(ROWS - 2, c + 1, SHORT[g], on ? C_TEXT : C_TEXT3, C_BG) + 2;
    }
    fill(ROWS - 1, 0, 1, COLS, C_BAR);
    if (g_status[0] && time(NULL) < g_statusUntil) put(ROWS - 1, 1, g_status, g_statusCol, C_BAR);
    else put(ROWS - 1, 1, "Up/Dn satellite  1-6 group  I ISS  L location  Q quit", C_TEXT3, C_BAR);
    put_r(ROWS - 1, COLS - 1, "Data: CelesTrak, NASA", C_TEXT3, C_BAR);
}

static void draw_all(bool fetch)
{
    (void)fetch;
    (void)g_msg;
    clear(C_BG);
    draw_header();
    draw_map();
    draw_list();
    draw_info();
    draw_keys();
    flush();
}

enum { K_NONE = -1, K_UP = 1000, K_DOWN, K_LEFT, K_RIGHT, K_HOME, K_ENTER, K_ESC, K_BACK, K_HANGUP };

/* A key, with arrow keys decoded; K_NONE after timeout_ms. */
static int read_key(int timeout_ms)
{
    int c = door_read_char_timeout(timeout_ms);
    if (c < 0) return K_NONE;
    if (c == 27) {
        int c2 = door_read_char_timeout(60);
        if (c2 < 0) return K_ESC;
        if (c2 == '[' || c2 == 'O') {
            int c3 = door_read_char_timeout(60);
            switch (c3) {
            case 'A': return K_UP;
            case 'B': return K_DOWN;
            case 'C': return K_RIGHT;
            case 'D': return K_LEFT;
            case 'H': return K_HOME;
            default:
                /* ESC [ 1 ~ and friends: eat to the terminator. */
                while (c3 >= 0 && c3 != '~' && !(c3 >= 'A' && c3 <= 'Z') && !(c3 >= 'a' && c3 <= 'z'))
                    c3 = door_read_char_timeout(60);
                return K_NONE;
            }
        }
        return K_ESC;
    }
    if (c == '\r' || c == '\n') return K_ENTER;
    if (c == 8 || c == 127) return K_BACK;
    return c;
}

/* The location picker: a box over the screen, typed search, arrow to choose.
 * Returns true when a place was picked (prefs updated). */
static bool pick_location(SatUserPrefs *p, bool firstRun)
{
    char query[SAT_SEARCH_LEN + 1] = "", last[SAT_SEARCH_LEN + 1] = "";
    int qlen = 0, sel = 0, count = -1;
    SatPlace places[SAT_MAX_PLACES];
    const char *msg = firstRun ? "Where are you? Type a city, \"City, State\" or a ZIP." :
                                 "Type a city, \"City, State\" or a ZIP / postcode.";
    const int r0 = 5, c0 = 14, h = 15, w = 52;
    for (;;) {
        draw_all(false);
        /* A drop shadow: darken what's to the right of and below the box. */
        for (int y = r0 + 1; y <= r0 + h; y++)
            for (int x = c0 + 2; x < c0 + w + 2; x++)
                if (y == r0 + h || x >= c0 + w)
                    if (y < ROWS && x < COLS) {
                        g_scr[y][x].bg = mix(g_scr[y][x].bg, 0x000000, 0.6f);
                        g_scr[y][x].fg = mix(g_scr[y][x].fg, 0x000000, 0.6f);
                    }
        fill(r0, c0, h, w, 0x18224A);
        for (int x = c0; x < c0 + w; x++) { setch(r0, x, '\xDF', C_ACCENT, 0x18224A); }
        put(r0 + 1, c0 + 3, "Choose a location", C_TEXT, KEEP);
        put(r0 + 2, c0 + 3, msg, C_TEXT3, KEEP);
        fill(r0 + 4, c0 + 3, 1, w - 6, 0x0B1124);
        put(r0 + 4, c0 + 4, qlen ? query : "Start typing...", qlen ? C_TEXT : C_TEXT3, KEEP);
        if (count < 0) {
            put(r0 + 7, c0 + 4, "Press Enter to search.", C_TEXT3, KEEP);
        } else if (count == 0) {
            put(r0 + 7, c0 + 4, "No places found.", C_TEXT2, KEEP);
            put(r0 + 8, c0 + 4, "Check the spelling, or try a nearby city or ZIP.", C_TEXT3, KEEP);
        } else {
            for (int i = 0; i < count && i < 7; i++) {
                bool on = i == sel;
                uint32_t bg = on ? C_PANEL_HI : KEEP;
                if (on) fill(r0 + 6 + i, c0 + 2, 1, w - 4, C_PANEL_HI);
                put(r0 + 6 + i, c0 + 3, on ? "\xAF" : " ", C_ACCENT, bg);
                char nm[44];
                snprintf(nm, sizeof nm, "%.42s", places[i].name);
                put(r0 + 6 + i, c0 + 5, nm, on ? C_TEXT : C_TEXT2, bg);
            }
        }
        bool choose = count > 0 && !strcmp(query, last);
        int c = put(r0 + h - 1, c0 + 3, " Enter ", C_TEXT, C_PANEL_HI);
        c = put(r0 + h - 1, c + 1, choose ? "choose" : "search", C_TEXT3, KEEP);
        if (count > 0) {
            c = put(r0 + h - 1, c + 3, " Up/Dn ", C_TEXT, C_PANEL_HI);
            c = put(r0 + h - 1, c + 1, "move", C_TEXT3, KEEP);
        }
        c = put(r0 + h - 1, c + 3, " Esc ", C_TEXT, C_PANEL_HI);
        put(r0 + h - 1, c + 1, "cancel", C_TEXT3, KEEP);
        flush();
        /* Show a cursor in the field while typing. */
        char at[32];
        snprintf(at, sizeof at, CSI "%d;%dH" CSI "?25h", r0 + 5, c0 + 5 + qlen);
        door_write(at);

        int k = read_key(300000);
        door_write(CSI "?25l");
        if (k == K_NONE || k == K_ESC) return false;
        if (k == K_UP && count > 0 && sel > 0) sel--;
        else if (k == K_DOWN && count > 0 && sel < count - 1 && sel < 6) sel++;
        else if (k == K_BACK) { if (qlen) query[--qlen] = 0; }
        else if (k == K_ENTER) {
            if (choose) {
                snprintf(p->place, sizeof p->place, "%s", places[sel].name);
                p->lat = places[sel].lat1e4 / 1e4;
                p->lon = places[sel].lon1e4 / 1e4;
                p->havePlace = true;
                int off;
                if (sat_utc_offset(p->lat, p->lon, &off)) p->utcOffset = off;
                sat_save_prefs(p);
                return true;
            }
            if (!qlen) continue;
            put(r0 + 7, c0 + 4, "Searching...                         ", C_WARN, 0x18224A);
            flush();
            count = sat_search(query, places, SAT_MAX_PLACES);
            snprintf(last, sizeof last, "%s", query);
            sel = 0;
            if (count < 0) { count = -1; msg = "Couldn't reach the place search. Try again."; }
        } else if (k < 127 && qlen < SAT_SEARCH_LEN &&
                   (isalnum(k) || k == ' ' || k == ',' || k == '.' || k == '-' || k == '\'')) {
            /* Only what a place name uses, as in the TRACE picker. */
            char ch = (char)k;
            if (ch == ' ' && qlen == 0) continue;
            if (ch >= 'a' && ch <= 'z' && (qlen == 0 || query[qlen - 1] == ' ')) ch = (char)(ch - 32);
            query[qlen++] = ch;
            query[qlen] = 0;
        }
    }
}


/* -------------------------------------------------------------- main --- */

void ansi_sats_run(SatUserPrefs *prefs)
{
    g_prefs = prefs;
    invalidate();
    door_write(CSI "0m" CSI "2J" CSI "?25l");
    clear(C_BG);
    put(10, 28, "Loading the satellites...", C_WARN, C_BG);
    flush();

    load_earth();
    if (!prefs->havePlace) pick_location(prefs, true);
    if (prefs->utcOffset < -900000 || prefs->havePlace) {
        int off;
        if (sat_utc_offset(prefs->lat, prefs->lon, &off)) { prefs->utcOffset = off; sat_save_prefs(prefs); }
    }
    load_group(prefs->group);
    invalidate();
    door_write(CSI "0m" CSI "2J");

    for (;;) {
        if (time(NULL) - g_passAt > 300) refresh_passes();
        draw_all(true);
        int k = read_key(1000);
        if (k == K_NONE) {
            if (door_time_remaining() <= 0) break;
            continue;
        }
        if (k == 'q' || k == 'Q' || k == K_ESC) break;
        int old = g_sel;
        switch (k) {
        case K_UP:   if (g_sel > 0) g_sel--; break;
        case K_DOWN: if (g_sel < g_n - 1) g_sel++; break;
        case K_HOME: g_sel = 0; break;
        case 'i': case 'I':
            for (int i = 0; i < g_n; i++) if (g_sats[i].e.norad == SAT_ISS) g_sel = i;
            if (g_sats[g_sel].e.norad != SAT_ISS && prefs->group != GRP_STATIONS) {
                prefs->group = GRP_STATIONS;
                prefs->selected = SAT_ISS;
                sat_save_prefs(prefs);
                load_group(GRP_STATIONS);
                old = -1;
            }
            break;
        case 'l': case 'L':
            if (pick_location(prefs, false)) g_passAt = 0;
            invalidate();
            door_write(CSI "0m" CSI "2J");
            break;
        default:
            if (k >= '1' && k < '1' + GRP_COUNT && k - '1' != prefs->group) {
                prefs->group = k - '1';
                sat_save_prefs(prefs);
                load_group(prefs->group);
                old = -1;
            }
            break;
        }
        if (g_n && g_sel != old) {
            prefs->selected = g_sats[g_sel].e.norad;
            sat_save_prefs(prefs);
            g_passAt = 0;
        }
    }
    door_write(CSI "0m" CSI "2J" CSI "H" CSI "?25h");
}
