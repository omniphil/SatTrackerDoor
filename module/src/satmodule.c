// The Satellite Tracker TRACE module: the picture half of the door.
//
// A live map of the Earth -- NASA's Blue Marble by day, Black Marble city lights
// by night, the terminator where the Sun sets right now -- with every satellite
// in the chosen group moving across it, the followed one's track and footprint,
// your home, and below it the satellite's numbers, its next passes over you and
// a sky chart of where to look.
//
// The door sends orbit elements; this module runs the same orbit code
// (orbit.h) every frame, so the satellites move smoothly without the door
// streaming positions. Mouse and keyboard drive everything.
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "trace_api.h"
#include "protocol.h"
#include "gfx.h"
#include "orbit.h"

#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#define STBI_NO_FAILURE_STRINGS
#define STBI_NO_THREAD_LOCALS
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-parameter"
#include "stb_image.h"
#pragma clang diagnostic pop

// ---------------------------------------------------------------- colours ---

#define C_BG       0x060A16
#define C_BAR      0x0C1328
#define C_PANEL    0x101830
#define C_PANEL_HI 0x1A2548
#define C_EDGE     0x243058
#define C_EDGE_HI  0x3A4C84
#define C_TEXT     0xEAF0FA
#define C_TEXT2    0x9AA6C6
#define C_TEXT3    0x5C688C
#define C_ACCENT   0x4FD1FF
#define C_MAGENTA  0xFF5FD2
#define C_SUN      0xFFC53D
#define C_SAT      0xFFE45C
#define C_SHADOW   0xC77B4A
#define C_TRACK    0x4FD1FF
#define C_HOME     0xFF4F6B
#define C_GOOD     0x6EDC8C
#define C_WARN     0xFFB547
#define C_ERR      0xFF6B6B

// ---------------------------------------------------------------- layout ---

#define BAR_H    28
#define MAP_X    0
#define MAP_Y    30
#define MAP_W    640
#define MAP_H    320
#define PANEL_Y  (MAP_Y + MAP_H + 2)
#define INFO_X   8
#define INFO_W   236
#define PASS_X   252
#define PASS_W   236
#define SKY_X    496
#define SKY_W    136

typedef struct { float x, y, w, h; } Rect;
static int inside(Rect r, float x, float y) { return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h; }

static const Rect R_CLOSE = { 608, 4, 24, 20 };
static Rect g_placeBtn = { 200, 4, 200, 20 };
static Rect g_chip[GRP_COUNT];
static const Rect R_PICKER = { 130, 72, 380, 330 };
#define PICK_FIELD_Y (R_PICKER.y + 62)
#define PICK_ROWS_Y  (R_PICKER.y + 108)
#define PICK_ROW_H   26
#define PICK_VISIBLE 6

// ------------------------------------------------------------------ state ---

static OrbSat   g_sats[SAT_MAX];
static int      g_n = 0, g_loading = 1;
static int      g_sel = -1, g_hover = -1;
static uint32_t g_want = SAT_ISS;             // the NORAD number to follow once a group arrives
static int      g_group = GRP_STATIONS;
static OrbPass  g_pass[4];
static int      g_nPass = 0, g_passFor = -1, g_passRow = 0;
static int32_t  g_passAt = -100000;

static uint32_t g_doorNow = 0;
static int32_t  g_recvMs = 0, g_utcOffset = 0;
static double   g_homeLat = 40.7128, g_homeLon = -74.0060;
static char     g_place[SAT_NAME_LEN] = "";

static char     g_status[128];
static int      g_statusKind = SAT_STATUS_INFO;
static int32_t  g_statusAt = -100000;

static int      g_quitting = 0, g_started = 0;
static float    g_mx = -1, g_my = -1;
static int      g_mouseOver = 0;

// Earth pictures.
static char      g_earthHash[65];
static uint32_t *g_day = NULL;                // BGRA, EW x EH
static uint8_t  *g_night = NULL;              // grey
static int       g_ew = 0, g_eh = 0;
static uint32_t *g_mapCache = NULL;           // the map rendered at device size
static int       g_cacheW = 0, g_cacheH = 0;
static int32_t   g_cacheAt = -100000;

// Place picker.
static SatPlace g_places[SAT_MAX_PLACES];
static int      g_placeCount = -1, g_picker = 0, g_pickSel = 0, g_searching = 0, g_firstRun = 0;
static char     g_query[SAT_SEARCH_LEN + 1], g_lastSearch[SAT_SEARCH_LEN + 1];
static int      g_qlen = 0;

static double now_utc(void)
{
    if (!g_doorNow) return 0;
    return (double)g_doorNow + (trace_time_ms() - g_recvMs) / 1000.0;
}

// ------------------------------------------------------------- door link ---

static void send_msg(uint8_t type, const void *payload, uint32_t len)
{
    uint8_t buf[sizeof(SatMsgHeader) + 64];
    if (len > 64) return;
    SatMsgHeader h = { type, 0, 1, len };
    memcpy(buf, &h, sizeof h);
    if (payload && len) memcpy(buf + sizeof h, payload, len);
    uint32_t total = sizeof h + len;
    if ((uint32_t)trace_send_room() >= total) trace_send(buf, (int32_t)total);
}

static void set_status(int kind, const char *s)
{
    snprintf(g_status, sizeof g_status, "%s", s);
    g_statusKind = kind;
    g_statusAt = trace_time_ms();
}

static void select_sat(int i)
{
    if (i < 0 || i >= g_n) return;
    g_sel = i;
    g_want = g_sats[i].e.norad;
    g_passFor = -1;                         // passes are worked out again
    g_passRow = 0;
    send_msg(SAT_OUT_SELECT, &g_want, 4);
}

static void set_group(int g)
{
    if (g < 0 || g >= GRP_COUNT || g == g_group) return;
    g_group = g;
    g_loading = 1;
    uint8_t b = (uint8_t)g;
    send_msg(SAT_OUT_GROUP, &b, 1);
}

// --------------------------------------------------------- Earth pictures ---

static uint8_t *read_asset(const char *hash, int32_t *size)
{
    *size = trace_asset_size(hash);
    if (*size <= 0) return NULL;
    uint8_t *buf = malloc((size_t)*size);
    for (int32_t off = 0; buf && off < *size; ) {
        int32_t got = trace_asset_read(hash, off, buf + off, *size - off);
        if (got <= 0) { free(buf); return NULL; }
        off += got;
    }
    return buf;
}

static void load_earth(void)
{
    int32_t size;
    uint8_t *raw = read_asset(g_earthHash, &size);
    if (!raw || size < 12 || memcmp(raw, "EAR1", 4)) { free(raw); return; }
    uint32_t dl, nl;
    memcpy(&dl, raw + 4, 4);
    memcpy(&nl, raw + 8, 4);
    if (12 + (int64_t)dl + nl > size) { free(raw); return; }
    int w, h, w2, h2, n;
    uint8_t *day = stbi_load_from_memory(raw + 12, (int)dl, &w, &h, &n, 3);
    uint8_t *night = stbi_load_from_memory(raw + 12 + dl, (int)nl, &w2, &h2, &n, 1);
    free(raw);
    if (day && night && w == w2 && h == h2) {
        g_day = malloc((size_t)w * h * 4);
        g_night = malloc((size_t)w * h);
        if (g_day && g_night) {
            for (int i = 0; i < w * h; i++)
                g_day[i] = (uint32_t)day[i * 3] << 16 | (uint32_t)day[i * 3 + 1] << 8 | day[i * 3 + 2];
            memcpy(g_night, night, (size_t)w * h);
            g_ew = w;
            g_eh = h;
        }
    }
    if (day) stbi_image_free(day);
    if (night) stbi_image_free(night);
}

// The map at device resolution: day and night blended by where the Sun is, with
// a faint graticule. Redrawn every half minute (the terminator creeps).
static void render_map_cache(void)
{
    int X0 = D(MAP_X), Y0 = D(MAP_Y), W = D(MAP_X + MAP_W) - X0, H = D(MAP_Y + MAP_H) - Y0;
    if (W != g_cacheW || H != g_cacheH) {
        free(g_mapCache);
        g_mapCache = malloc((size_t)W * H * 4);
        g_cacheW = W;
        g_cacheH = H;
    }
    if (!g_mapCache) return;
    double t = now_utc(), slat = 0, slon = 0;
    if (t) orb_subsolar(t, &slat, &slon);
    double ss = sin(slat), cs = cos(slat);
    // Per column: cos(longitude - subsolar longitude); per row: sin/cos latitude.
    float *colc = malloc(sizeof(float) * (size_t)W);
    for (int x = 0; x < W && colc; x++) {
        double lon = ((x + 0.5) / W * 360.0 - 180.0) * ORB_DEG;
        colc[x] = (float)cos(lon - slon);
    }
    for (int y = 0; y < H; y++) {
        double lat = (90.0 - (y + 0.5) / H * 180.0) * ORB_DEG;
        float sl = (float)(sin(lat) * ss), cl = (float)(cos(lat) * cs);
        float fy = (y + 0.5f) / H * g_eh - 0.5f;
        int y0 = (int)floorf(fy);
        float ty = fy - y0;
        if (y0 < 0) { y0 = 0; ty = 0; }
        int y1 = y0 + 1 < g_eh ? y0 + 1 : y0;
        uint32_t *row = g_mapCache + y * W;
        for (int x = 0; x < W; x++) {
            uint32_t day = 0x0B1E3C, lights = 0;
            if (g_day) {
                float fx = (x + 0.5f) / W * g_ew - 0.5f;
                int x0 = (int)floorf(fx);
                float tx = fx - x0;
                if (x0 < 0) { x0 = 0; tx = 0; }
                int x1 = x0 + 1 < g_ew ? x0 + 1 : x0;
                uint32_t a = mixf(g_day[y0 * g_ew + x0], g_day[y0 * g_ew + x1], tx);
                uint32_t b = mixf(g_day[y1 * g_ew + x0], g_day[y1 * g_ew + x1], tx);
                day = mixf(a, b, ty);
                float na = g_night[y0 * g_ew + x0] + (g_night[y0 * g_ew + x1] - g_night[y0 * g_ew + x0]) * tx;
                float nb = g_night[y1 * g_ew + x0] + (g_night[y1 * g_ew + x1] - g_night[y1 * g_ew + x0]) * tx;
                lights = (uint32_t)(na + (nb - na) * ty);
            }
            float cz = sl + cl * (colc ? colc[x] : 1);            // the Sun's height: + day, - night
            float d = clampf((cz + 0.06f) / 0.12f, 0, 1);
            d = d * d * (3 - 2 * d);
            uint32_t night = mix(mix(day, 0x020410, 215), 0xFFC66A, (int)(lights * 0.9f));
            row[x] = mixf(night, day, d) | 0xFF000000u;
        }
    }
    free(colc);
    // Graticule every 30 degrees, very faint.
    for (int k = 1; k < 12; k++) {
        int x = (int)((float)k / 12 * W);
        for (int y = 0; y < H; y += 2) g_mapCache[y * W + x] = mix(g_mapCache[y * W + x], 0x8FA8D8, 40) | 0xFF000000u;
    }
    for (int k = 1; k < 6; k++) {
        int y = (int)((float)k / 6 * H);
        for (int x = 0; x < W; x += 2) g_mapCache[y * W + x] = mix(g_mapCache[y * W + x], 0x8FA8D8, k == 3 ? 70 : 40) | 0xFF000000u;
    }
    g_cacheAt = trace_time_ms();
}

static void draw_map_base(void)
{
    if (!g_mapCache || trace_time_ms() - g_cacheAt > 30000 || g_cacheW != D(MAP_X + MAP_W) - D(MAP_X)) render_map_cache();
    if (!g_mapCache) return;
    int X0 = D(MAP_X), Y0 = D(MAP_Y);
    for (int y = 0; y < g_cacheH && Y0 + y < g_H; y++)
        memcpy(g_frame + (Y0 + y) * g_W + X0, g_mapCache + y * g_cacheW, (size_t)g_cacheW * 4);
}

// ------------------------------------------------------------ map drawing ---

static float map_x(double lon) { return MAP_X + (float)((lon / ORB_DEG + 180.0) / 360.0 * MAP_W); }
static float map_y(double lat) { return MAP_Y + (float)((90.0 - lat / ORB_DEG) / 180.0 * MAP_H); }

// A polyline of lat/lon points, broken where it crosses the date line.
static void geo_line(const double *lat, const double *lon, int n, float w, uint32_t col, int a)
{
    for (int i = 1; i < n; i++) {
        float x0 = map_x(lon[i - 1]), y0 = map_y(lat[i - 1]), x1 = map_x(lon[i]), y1 = map_y(lat[i]);
        if (fabsf(x1 - x0) > MAP_W / 2) continue;
        line_l(x0, y0, x1, y1, w, col, a);
    }
}

// The footprint: the ring on the ground from which the satellite is above the horizon.
static void footprint(double lat, double lon, double alt)
{
    double rho = acos(ORB_RE / (ORB_RE + alt));
    double la[73], lo[73];
    for (int i = 0; i <= 72; i++) {
        double th = i * ORB_TWOPI / 72;
        double l2 = asin(sin(lat) * cos(rho) + cos(lat) * sin(rho) * cos(th));
        double o2 = lon + atan2(sin(th) * sin(rho) * cos(lat), cos(rho) - sin(lat) * sin(l2));
        o2 = fmod(o2 + ORB_PI, ORB_TWOPI);
        if (o2 < 0) o2 += ORB_TWOPI;
        la[i] = l2;
        lo[i] = o2 - ORB_PI;
    }
    geo_line(la, lo, 73, 1.4f, C_TRACK, 170);
}

static void home_marker(void)
{
    float x = map_x(g_homeLon * ORB_DEG), y = map_y(g_homeLat * ORB_DEG);
    circle_l(x, y, 6, C_HOME, 60);
    circle_l(x, y, 3.2f, C_HOME, 255);
    circle_l(x, y, 1.3f, 0xFFFFFF, 255);
}

static void draw_satellites(void)
{
    double t = now_utc();
    if (!t) return;
    OrbVec sun = orb_sun(t);
    // The followed satellite: track (the last half orbit dim, the next orbit
    // bright) and footprint, under the dots.
    if (g_sel >= 0) {
        OrbSat *s = &g_sats[g_sel];
        double period = 1440.0 / s->e.nRevDay * 60.0;
        static double la[260], lo[260];
        int n = 0;
        for (double u = t - period * 0.5; u <= t && n < 130; u += period / 120) {
            double h;
            if (orb_where(s, u, &la[n], &lo[n], &h, NULL)) n++;
        }
        geo_line(la, lo, n, 1.2f, C_TRACK, 90);
        n = 0;
        for (double u = t; u <= t + period && n < 250; u += period / 240) {
            double h;
            if (orb_where(s, u, &la[n], &lo[n], &h, NULL)) n++;
        }
        geo_line(la, lo, n, 1.8f, C_TRACK, 220);
        double a, b, h;
        if (orb_where(s, t, &a, &b, &h, NULL)) footprint(a, b, h);
    }
    home_marker();
    for (int i = 0; i < g_n; i++) {
        if (i == g_sel) continue;
        double la, lo, h;
        OrbVec r;
        if (!orb_where(&g_sats[i], t, &la, &lo, &h, &r)) continue;
        float x = map_x(lo), y = map_y(la);
        int lit = orb_sunlit(r, sun);
        uint32_t col = lit ? C_SAT : C_SHADOW;
        if (i == g_hover) circle_l(x, y, 7, 0xFFFFFF, 60);
        circle_l(x, y, 4.2f, col, 55);
        circle_l(x, y, 2.2f, col, 255);
    }
    if (g_sel >= 0) {
        double la, lo, h;
        if (orb_where(&g_sats[g_sel], t, &la, &lo, &h, NULL)) {
            float x = map_x(lo), y = map_y(la);
            float pulse = 0.5f + 0.5f * sinf(trace_time_ms() / 250.0f);
            glow_l(x, y, 16, 0xFFFFFF, (int)(70 + 50 * pulse));
            ring_l(x, y, 7 + 2 * pulse, 1.4f, 0xFFFFFF, 220);
            circle_l(x, y, 3.4f, 0xFFFFFF, 255);
            // Its name beside it, flipped to stay on the map.
            const char *nm = g_sats[g_sel].e.name;
            float w = text_w(F_BOLD, nm, 0) + 10;
            float lx = x + 12, ly = y - 8;
            if (lx + w > MAP_X + MAP_W - 4) lx = x - 12 - w;
            if (ly < MAP_Y + 30) ly = y + 8;
            rrect_l(lx, ly, w, 17, 4, 0x000000, 150);
            text(F_BOLD, lx + 5, ly + 1, nm, 0xFFFFFF);
        }
    }
    // The hovered one's name.
    if (g_hover >= 0 && g_hover != g_sel) {
        double la, lo, h;
        if (orb_where(&g_sats[g_hover], t, &la, &lo, &h, NULL)) {
            float x = map_x(lo), y = map_y(la);
            char buf[64];
            snprintf(buf, sizeof buf, "%s  %.0f km", g_sats[g_hover].e.name, h);
            float w = text_w(F_SMALL, buf, 0) + 10;
            float lx = x + 10, ly = y + 6;
            if (lx + w > MAP_X + MAP_W - 4) lx = x - 10 - w;
            if (ly > MAP_Y + MAP_H - 20) ly = y - 20;
            rrect_l(lx, ly, w, 15, 4, 0x000000, 170);
            text(F_SMALL, lx + 5, ly + 2, buf, C_TEXT);
        }
    }
}

// The satellite nearest the pointer, within 9 pixels.
static int sat_at(float mx, float my)
{
    double t = now_utc();
    int best = -1;
    float bd = 81;
    for (int i = 0; i < g_n; i++) {
        double la, lo, h;
        if (!orb_where(&g_sats[i], t, &la, &lo, &h, NULL)) continue;
        float dx = map_x(lo) - mx, dy = map_y(la) - my, d = dx * dx + dy * dy;
        if (d < bd) { bd = d; best = i; }
    }
    return best;
}

// ------------------------------------------------------------- the chips ---

static void draw_chips(void)
{
    float x = MAP_X + 8, y = MAP_Y + 8;
    for (int g = 0; g < GRP_COUNT; g++) {
        const char *nm = SAT_GROUP_NAME[g];
        float w = text_w(F_BOLD, nm, 0) + 18;
        g_chip[g] = (Rect){ x, y, w, 20 };
        int on = g == g_group, hot = g_mouseOver && !g_picker && inside(g_chip[g], g_mx, g_my);
        rrect_l(x, y, w, 20, 10, on ? C_ACCENT : 0x000000, on ? 235 : hot ? 200 : 140);
        rrect_stroke_l(x, y, w, 20, 10, 1, on ? C_ACCENT : hot ? C_TEXT2 : C_EDGE_HI, 255);
        text(F_BOLD, x + 9, y + 3, nm, on ? 0x001020 : hot ? C_TEXT : C_TEXT2);
        x += w + 6;
    }
    if (g_loading) {
        float t = trace_time_ms() / 1000.0f;
        for (int i = 0; i < 8; i++) {
            float ang = i * PI_F / 4 + t * 5;
            circle_l(x + 12 + cosf(ang) * 6, y + 10 + sinf(ang) * 6, 1.5f, C_TEXT, 60 + i * 25);
        }
    }
}

// ------------------------------------------------------------- the panel ---

static void fmt_local(char *out, size_t n, double t, int withDay)
{
    int64_t lt = (int64_t)t + g_utcOffset;
    int64_t days = lt >= 0 ? lt / 86400 : (lt - 86399) / 86400, secs = lt - days * 86400;
    static const char *const WD[7] = { "Thu", "Fri", "Sat", "Sun", "Mon", "Tue", "Wed" };
    int hh = (int)(secs / 3600), mm = (int)(secs % 3600 / 60);
    if (withDay) snprintf(out, n, "%s %02d:%02d", WD[((days % 7) + 7) % 7], hh, mm);
    else snprintf(out, n, "%02d:%02d", hh, mm);
}

static void draw_info(void)
{
    rrect_l(INFO_X, PANEL_Y + 4, INFO_W, 118, 8, C_PANEL, 255);
    rrect_stroke_l(INFO_X, PANEL_Y + 4, INFO_W, 118, 8, 1, C_EDGE, 255);
    if (g_sel < 0) {
        text(F_BODY, INFO_X + 12, PANEL_Y + 52, g_loading ? "Fetching orbits..." : "Click a satellite to follow it.", C_TEXT2);
        return;
    }
    OrbSat *s = &g_sats[g_sel];
    double t = now_utc(), la, lo, h;
    OrbVec r, r2;
    char buf[96];
    char nm[64];
    fit(F_HEAD, s->e.name, INFO_W - 24, nm, sizeof nm);
    text(F_HEAD, INFO_X + 12, PANEL_Y + 10, nm, C_TEXT);
    float y = PANEL_Y + 34;
    if (orb_where(s, t, &la, &lo, &h, &r) && orb_where(s, t + 1, &la, &lo, &h, &r2)) {
        orb_where(s, t, &la, &lo, &h, &r);
        double v = sqrt((r2.x - r.x) * (r2.x - r.x) + (r2.y - r.y) * (r2.y - r.y) + (r2.z - r.z) * (r2.z - r.z));
        struct { const char *k; char v[48]; } rows[4];
        rows[0].k = "Altitude";  snprintf(rows[0].v, sizeof rows[0].v, "%.0f km  (%.0f mi)", h, h * 0.621371);
        rows[1].k = "Speed";     snprintf(rows[1].v, sizeof rows[1].v, "%.2f km/s  (%.0f mph)", v, v * 2236.94);
        rows[2].k = "Over";      snprintf(rows[2].v, sizeof rows[2].v, "%.1f\xb0%c  %.1f\xb0%c", fabs(la / ORB_DEG), la >= 0 ? 'N' : 'S',
                                          fabs(lo / ORB_DEG), lo >= 0 ? 'E' : 'W');
        rows[3].k = "Orbit";     snprintf(rows[3].v, sizeof rows[3].v, "%.0f min, %.1f\xb0 incl.", 1440.0 / s->e.nRevDay, s->e.incl / ORB_DEG);
        for (int i = 0; i < 4; i++) {
            label(INFO_X + 12, y + 2, rows[i].k, C_TEXT3);
            text(F_BODY, INFO_X + 80, y, rows[i].v, C_TEXT);
            y += 17;
        }
        int lit = orb_sunlit(r, orb_sun(t));
        rrect_l(INFO_X + 12, y + 2, lit ? 84 : 118, 15, 7.5f, lit ? C_SUN : 0x3A3050, lit ? 60 : 255);
        text(F_SMALL, INFO_X + 20, y + 3, lit ? "IN SUNLIGHT" : "IN EARTH'S SHADOW", lit ? C_SUN : 0xC8B8FF);
    }
    snprintf(buf, sizeof buf, "#%u", s->e.norad);
    text_r(F_SMALL, INFO_X + INFO_W - 12, PANEL_Y + 14, buf, C_TEXT3);
}

static void refresh_passes(void)
{
    if (g_sel < 0 || !now_utc()) return;
    g_nPass = orb_passes(&g_sats[g_sel], g_homeLat * ORB_DEG, g_homeLon * ORB_DEG, 0.05, now_utc(), 72, 10, g_pass, 4);
    g_passFor = g_sel;
    g_passAt = trace_time_ms();
    if (g_passRow >= g_nPass) g_passRow = 0;
}

static Rect pass_row(int i) { return (Rect){ PASS_X + 6, PANEL_Y + 32 + i * 22, PASS_W - 12, 20 }; }

static void draw_passes(void)
{
    rrect_l(PASS_X, PANEL_Y + 4, PASS_W, 118, 8, C_PANEL, 255);
    rrect_stroke_l(PASS_X, PANEL_Y + 4, PASS_W, 118, 8, 1, C_EDGE, 255);
    char head[96], place[48];
    fit(F_BOLD, g_place[0] ? g_place : "your place", PASS_W - 110, place, sizeof place);
    snprintf(head, sizeof head, "Passes over %s", place);
    text(F_BOLD, PASS_X + 10, PANEL_Y + 11, head, C_ACCENT);
    if (g_sel < 0) return;
    if (g_sats[g_sel].deep) {
        text(F_BODY, PASS_X + 10, PANEL_Y + 44, "A high orbit: it doesn't pass", C_TEXT2);
        text(F_BODY, PASS_X + 10, PANEL_Y + 60, "over, it hangs or drifts slowly.", C_TEXT2);
        return;
    }
    if (!g_nPass) {
        text(F_BODY, PASS_X + 10, PANEL_Y + 50, "None above 10\xb0 in the next 3 days.", C_TEXT2);
        return;
    }
    for (int i = 0; i < g_nPass; i++) {
        const OrbPass *p = &g_pass[i];
        Rect r = pass_row(i);
        int on = i == g_passRow, hot = g_mouseOver && !g_picker && inside(r, g_mx, g_my);
        if (on || hot) rrect_l(r.x, r.y, r.w, r.h, 5, on ? C_PANEL_HI : 0x151F3C, 255);
        char when[32], el[16], dir[24];
        fmt_local(when, sizeof when, p->aos, 1);
        snprintf(el, sizeof el, "%.0f\xb0", p->maxEl / ORB_DEG);
        snprintf(dir, sizeof dir, "%s-%s", orb_compass(p->azAos), orb_compass(p->azLos));
        text(F_BODY, r.x + 6, r.y + 3, when, on ? C_TEXT : C_TEXT2);
        text(F_BOLD, r.x + 84, r.y + 3, el, C_TEXT);
        text(F_SMALL, r.x + 112, r.y + 5, dir, C_TEXT3);
        if (p->visible) {
            rrect_l(r.x + r.w - 58, r.y + 3, 54, 14, 7, C_GOOD, 230);
            text_c(F_SMALL, r.x + r.w - 31, r.y + 4, "VISIBLE", 0x06240F);
        }
    }
}

// Where to look: the chosen pass as a path across the sky (the edge is the
// horizon, the middle is straight up, north at the top).
static void draw_sky(void)
{
    rrect_l(SKY_X, PANEL_Y + 4, SKY_W, 118, 8, C_PANEL, 255);
    rrect_stroke_l(SKY_X, PANEL_Y + 4, SKY_W, 118, 8, 1, C_EDGE, 255);
    float cx = SKY_X + SKY_W / 2, cy = PANEL_Y + 66, R = 44;
    circle_l(cx, cy, R, 0x0A1024, 255);
    ring_l(cx, cy, R, 1.2f, C_EDGE_HI, 255);
    ring_l(cx, cy, R * 2 / 3, 1, C_EDGE, 255);
    ring_l(cx, cy, R / 3, 1, C_EDGE, 255);
    text_c(F_SMALL, cx, cy - R - 13, "N", C_TEXT2);
    text_c(F_SMALL, cx, cy + R + 1, "S", C_TEXT2);
    text(F_SMALL, cx + R + 3, cy - 6, "E", C_TEXT2);
    text_r(F_SMALL, cx - R - 3, cy - 6, "W", C_TEXT2);
    if (g_sel < 0 || g_sats[g_sel].deep || !g_nPass) {
        text_c(F_SMALL, cx, cy - 6, "no pass", C_TEXT3);
        return;
    }
    const OrbPass *p = &g_pass[g_passRow];
    OrbSat *s = &g_sats[g_sel];
    OrbVec obs = orb_observer_ecef(g_homeLat * ORB_DEG, g_homeLon * ORB_DEG, 0.05);
    float px = 0, py = 0;
    int first = 1;
    for (double u = p->aos; u <= p->los + 1; u += 8) {
        OrbVec r;
        if (!orb_propagate(s, u, &r)) break;
        double az, el;
        orb_look(g_homeLat * ORB_DEG, g_homeLon * ORB_DEG, obs, orb_teme_to_ecef(r, orb_gmst(u)), &az, &el, NULL);
        if (el < 0) el = 0;
        float rr = R * (float)(1 - el / (ORB_PI / 2));
        float x = cx + rr * (float)sin(az), y = cy - rr * (float)cos(az);
        if (!first) line_l(px, py, x, y, 2, p->visible ? C_GOOD : C_ACCENT, 255);
        if (first) circle_l(x, y, 2.5f, C_TEXT, 255);
        px = x;
        py = y;
        first = 0;
    }
    // An arrowhead where it sets.
    if (!first) circle_l(px, py, 3, p->visible ? C_GOOD : C_ACCENT, 255);
    char when[16];
    fmt_local(when, sizeof when, p->tmax, 0);
    text_r(F_SMALL, SKY_X + SKY_W - 6, PANEL_Y + 8, when, C_TEXT3);
}

// ------------------------------------------------------------------ chrome ---

static void pin_icon(float cx, float cy, float s, uint32_t col)
{
    circle_l(cx, cy - s * 0.12f, s * 0.3f, col, 255);
    float p[] = { cx - s * 0.26f, cy - s * 0.02f, cx, cy + s * 0.48f, cx + s * 0.26f, cy - s * 0.02f };
    poly_l(p, 3, col, 255);
    circle_l(cx, cy - s * 0.12f, s * 0.11f, C_BAR, 255);
}

static void draw_bar(void)
{
    fill_l(0, 0, LW, BAR_H, C_BAR, 255);
    fill_l(0, BAR_H, LW, 1, C_EDGE, 255);
    // A small orbit logo: a planet and a ring with a dot riding it.
    float t = trace_time_ms() / 1000.0f;
    circle_l(21, 14, 6, 0x3A7BD5, 255);
    ring_l(21, 14, 10, 1.2f, C_ACCENT, 200);
    circle_l(21 + cosf(t) * 10, 14 + sinf(t) * 10, 2, 0xFFFFFF, 255);
    float x = text(F_HEAD, 38, 5, "TERM", C_MAGENTA);
    x = text(F_HEAD, x, 5, "inator", C_ACCENT);
    x = text(F_HEAD, x + 6, 5, "Satellite Tracker", C_TEXT);

    // The place gets what room is left before the clocks.
    char name[64];
    float room = R_CLOSE.x - 190 - (x + 14) - 42;
    if (room < 60) room = 60;
    fit(F_BODY, g_place[0] ? g_place : "Choose a location", room, name, sizeof name);
    float bw = text_w(F_BODY, name, 0) + 42;
    g_placeBtn = (Rect){ x + 14, 4, bw, 20 };
    int hot = !g_picker && g_mouseOver && inside(g_placeBtn, g_mx, g_my);
    rrect_l(g_placeBtn.x, 4, bw, 20, 10, hot ? C_PANEL_HI : C_PANEL, 255);
    rrect_stroke_l(g_placeBtn.x, 4, bw, 20, 10, 1, hot ? C_ACCENT : C_EDGE, 255);
    pin_icon(g_placeBtn.x + 13, 14, 12, C_HOME);
    text(F_BODY, g_placeBtn.x + 22, 7, name, C_TEXT);
    float chx = g_placeBtn.x + bw - 12;
    line_l(chx - 3, 12, chx, 15, 1.4f, C_TEXT2, 255);
    line_l(chx, 15, chx + 3, 12, 1.4f, C_TEXT2, 255);

    if (g_doorNow) {
        char lt[24], utc[16];
        fmt_local(lt, sizeof lt, now_utc(), 1);
        int64_t u = (int64_t)now_utc() % 86400;
        snprintf(utc, sizeof utc, "%02d:%02dZ", (int)(u / 3600), (int)(u % 3600 / 60));
        text_r(F_BODY, R_CLOSE.x - 10, 7, utc, C_TEXT3);
        text_r(F_BODY, R_CLOSE.x - 18 - text_w(F_BODY, utc, 0), 7, lt, C_TEXT2);
    }
    int hotC = !g_picker && g_mouseOver && inside(R_CLOSE, g_mx, g_my);
    if (hotC) rrect_l(R_CLOSE.x, R_CLOSE.y, R_CLOSE.w, R_CLOSE.h, 6, 0x5A2030, 255);
    float r = 20 * 0.22f, cx = R_CLOSE.x + 12, cy = R_CLOSE.y + 10;
    line_l(cx - r, cy - r, cx + r, cy + r, 1.8f, hotC ? 0xFFFFFF : C_TEXT2, 255);
    line_l(cx - r, cy + r, cx + r, cy - r, 1.8f, hotC ? 0xFFFFFF : C_TEXT2, 255);
}

// A status line over the bottom of the map, for a few seconds.
static void draw_status(void)
{
    int32_t now = trace_time_ms();
    if (!g_status[0] || (g_statusKind != SAT_STATUS_BUSY && now - g_statusAt > 4000)) {
        text_r(F_SMALL, MAP_X + MAP_W - 8, MAP_Y + MAP_H - 15, "Orbits: CelesTrak   Earth: NASA", 0xB8C4DA);
        return;
    }
    uint32_t col = g_statusKind == SAT_STATUS_ERROR ? C_ERR : g_statusKind == SAT_STATUS_BUSY ? C_WARN : C_GOOD;
    float w = text_w(F_BODY, g_status, 0) + 16;
    rrect_l(MAP_X + MAP_W - 8 - w, MAP_Y + MAP_H - 24, w, 18, 9, 0x000000, 170);
    text(F_BODY, MAP_X + MAP_W - w, MAP_Y + MAP_H - 22, g_status, col);
}

// ------------------------------------------------------------------ picker ---

static void spinner(float cx, float cy, float r, uint32_t col)
{
    float t = trace_time_ms() / 1000.0f;
    for (int i = 0; i < 8; i++) {
        float ang = i * PI_F / 4 + t * 5;
        circle_l(cx + cosf(ang) * r, cy + sinf(ang) * r, r * 0.22f, col, 60 + (int)(195 * (i / 7.0f)));
    }
}

static float keycap(float x, float y, const char *k)
{
    float w = text_w(F_SMALL, k, 0) + 8;
    if (w < 14) w = 14;
    rrect_l(x, y, w, 13, 3, C_PANEL_HI, 255);
    rrect_stroke_l(x, y, w, 13, 3, 1, C_EDGE_HI, 255);
    text_c(F_SMALL, x + w / 2, y + 1, k, C_TEXT2);
    return x + w;
}

static float hint(float x, float y, const char *k, const char *what)
{
    x = keycap(x, y, k) + 4;
    return text(F_SMALL, x, y + 1, what, C_TEXT3) + 12;
}

static int picker_row_at(float mx, float my)
{
    const Rect *P = &R_PICKER;
    if (mx < P->x + 16 || mx > P->x + P->w - 16) return -1;
    int i = (int)((my - PICK_ROWS_Y) / PICK_ROW_H);
    if (my < PICK_ROWS_Y || i < 0 || i >= PICK_VISIBLE || i >= g_placeCount) return -1;
    return i;
}

static void draw_picker(void)
{
    fill_d(0, 0, g_W, g_H, 0x02040A, 175);
    const Rect *P = &R_PICKER;
    rrect_l(P->x + 3, P->y + 5, P->w, P->h, 12, 0x000000, 110);
    rrect_l(P->x, P->y, P->w, P->h, 12, 0x131B38, 255);
    rrect_stroke_l(P->x, P->y, P->w, P->h, 12, 1, C_EDGE_HI, 255);
    pin_icon(P->x + 26, P->y + 26, 18, C_HOME);
    text(F_HEAD, P->x + 42, P->y + 16, "Where do you watch from?", C_TEXT);
    text(F_SMALL, P->x + 42, P->y + 38, "A city, \"City, State\", or a ZIP / postcode", C_TEXT3);
    float fx = P->x + 16, fy = PICK_FIELD_Y, fw = P->w - 32, fh = 34;
    rrect_l(fx, fy, fw, fh, 8, 0x0B1124, 255);
    rrect_stroke_l(fx, fy, fw, fh, 8, 1.5f, C_ACCENT, 255);
    ring_l(fx + 17, fy + 15, 5.5f, 1.8f, C_TEXT2, 255);
    line_l(fx + 21, fy + 19, fx + 25, fy + 23, 2, C_TEXT2, 255);
    float tx = fx + 34;
    if (g_qlen) tx = text(F_HEAD, tx, fy + 8, g_query, C_TEXT);
    else text(F_HEAD, tx, fy + 8, "Start typing...", C_TEXT3);
    if ((trace_time_ms() / 530) % 2 == 0) fill_l(g_qlen ? tx + 1 : fx + 33, fy + 8, 1.5f, 18, C_ACCENT, 255);
    float ry = PICK_ROWS_Y;
    if (g_searching) {
        spinner(P->x + P->w / 2 - 44, ry + 40, 6, C_TEXT2);
        text(F_BODY, P->x + P->w / 2 - 32, ry + 33, "Searching...", C_TEXT2);
    } else if (g_placeCount == 0) {
        text_c(F_BODY, P->x + P->w / 2, ry + 26, "No places found.", C_TEXT2);
        text_c(F_SMALL, P->x + P->w / 2, ry + 46, "Check the spelling, or try a nearby city or a ZIP.", C_TEXT3);
    } else if (g_placeCount < 0) {
        text_c(F_SMALL, P->x + P->w / 2, ry + 30, "Press Enter to search.", C_TEXT3);
    } else {
        for (int i = 0; i < g_placeCount && i < PICK_VISIBLE; i++) {
            float y = ry + i * PICK_ROW_H;
            int sel = i == g_pickSel;
            if (sel) {
                rrect_l(P->x + 16, y + 1, P->w - 32, PICK_ROW_H - 2, 7, C_PANEL_HI, 255);
                rrect_stroke_l(P->x + 16, y + 1, P->w - 32, PICK_ROW_H - 2, 7, 1, C_EDGE_HI, 255);
            }
            pin_icon(P->x + 32, y + 13, 11, sel ? C_HOME : C_TEXT3);
            char nm[64];
            fit(F_BODY, g_places[i].name, P->w - 110, nm, sizeof nm);
            text(F_BODY, P->x + 44, y + 6, nm, sel ? C_TEXT : C_TEXT2);
        }
    }
    float hy = P->y + P->h - 26;
    fill_l(P->x + 1, hy - 8, P->w - 2, 1, C_EDGE, 255);
    float hx = P->x + 18;
    hx = hint(hx, hy, "Enter", g_placeCount > 0 && !strcmp(g_query, g_lastSearch) ? "choose" : "search");
    if (g_placeCount > 0) hx = hint(hx, hy, "Up/Dn", "move");
    hint(hx, hy, "Esc", g_firstRun ? "skip" : "cancel");
}

static void open_picker(void)
{
    g_picker = 1;
    g_qlen = 0;
    g_query[0] = g_lastSearch[0] = 0;
    g_placeCount = -1;
    g_pickSel = 0;
    g_searching = 0;
}

static void do_search(void)
{
    if (!g_qlen) return;
    uint8_t buf[1 + SAT_SEARCH_LEN];
    buf[0] = (uint8_t)g_qlen;
    memcpy(buf + 1, g_query, (size_t)g_qlen);
    send_msg(SAT_OUT_SEARCH, buf, 1 + (uint32_t)g_qlen);
    snprintf(g_lastSearch, sizeof g_lastSearch, "%s", g_query);
    g_searching = 1;
    g_placeCount = -1;
    g_pickSel = 0;
}

static void do_pick(int i)
{
    if (i < 0 || i >= g_placeCount) return;
    uint8_t b = (uint8_t)i;
    send_msg(SAT_OUT_PICK, &b, 1);
    g_picker = 0;
    g_firstRun = 0;
    set_status(SAT_STATUS_BUSY, "Setting your place...");
}

// ------------------------------------------------------------- TRACE API ---

static void set_scale(int s)
{
    if (s == g_S && g_frame) return;
    uint32_t *nf = malloc((size_t)(LW * s) * (size_t)(LH * s) * 4);
    if (!nf) { if (g_frame) return; s = 1; nf = malloc(LW * LH * 4); }
    free(g_frame);
    g_frame = nf;
    g_S = s;
    g_W = LW * s;
    g_H = LH * s;
    clip_all();
}

int32_t trace_init(void)
{
    set_scale(1);
    // Needs TERMinator 1.1.3+; the door only offers TRACE when mouse=1.
    trace_mouse_mode(TRACE_MOUSE_POINTER);
    trace_set_tick(30);
    return 0;
}

void *trace_alloc(int32_t size) { return size > 0 ? malloc((size_t)size) : NULL; }
void  trace_free(void *ptr)     { free(ptr); }

void trace_on_resize(int32_t width, int32_t height)
{
    set_display(width, height);
}

void trace_on_data(const char *data, int32_t length)
{
    if (length <= 0) return;
    if (length > 6 && !memcmp(data, "earth=", 6)) {
        int n = length - 6 > 64 ? 64 : length - 6;
        memcpy(g_earthHash, data + 6, (size_t)n);
        g_earthHash[n] = 0;
        if (!g_day) load_earth();
        g_cacheAt = -100000;
        return;
    }
    if (length == 5 && !memcmp(data, "start", 5)) {
        if (!g_started) { g_started = 1; send_msg(SAT_OUT_READY, NULL, 0); }
        return;
    }
    if (length == 4 && !memcmp(data, "quit", 4)) { g_quitting = 1; return; }

    const char *nl = memchr(data, '\n', (size_t)length);
    if (!nl) return;
    const uint8_t *body = (const uint8_t *)nl + 1;
    int32_t bodyLen = length - (int32_t)(body - (const uint8_t *)data);
    if (bodyLen < (int32_t)sizeof(SatMsgHeader)) return;
    SatMsgHeader h;
    memcpy(&h, body, sizeof h);
    const uint8_t *p = body + sizeof h;
    if ((uint32_t)(bodyLen - (int32_t)sizeof h) < h.bytes) return;

    switch (h.type) {
    case SAT_IN_PREFS:
        if (h.bytes >= sizeof(SatPrefs)) {
            SatPrefs pr;
            memcpy(&pr, p, sizeof pr);
            g_doorNow = pr.doorNow;
            g_recvMs = trace_time_ms();
            g_utcOffset = pr.utcOffset;
            g_homeLat = pr.lat1e4 / 1e4;
            g_homeLon = pr.lon1e4 / 1e4;
            if (pr.selected) g_want = pr.selected;
            if (pr.group < GRP_COUNT) g_group = pr.group;
            memcpy(g_place, pr.place, SAT_NAME_LEN);
            g_place[SAT_NAME_LEN - 1] = 0;
            g_passFor = -1;                 // a new home: new passes
            g_cacheAt = -100000;
            if (pr.firstRun && !g_picker) { g_firstRun = 1; open_picker(); }
        }
        break;
    case SAT_IN_ELEMS: {
        if (h.flags & 1) { g_n = 0; g_sel = -1; g_hover = -1; g_loading = 1; }
        int n = h.count;
        if ((uint32_t)n * sizeof(SatWire) > h.bytes) n = (int)(h.bytes / sizeof(SatWire));
        for (int i = 0; i < n && g_n < SAT_MAX; i++) {
            SatWire w;
            memcpy(&w, p + i * sizeof w, sizeof w);
            OrbElements e;
            memset(&e, 0, sizeof e);
            e.epoch = w.epoch; e.incl = w.incl; e.raan = w.raan; e.ecc = w.ecc;
            e.argp = w.argp; e.mo = w.mo; e.nRevDay = w.nRevDay; e.bstar = w.bstar;
            e.norad = w.norad;
            memcpy(e.name, w.name, 24);
            e.name[24] = 0;
            orb_init(&g_sats[g_n++], &e);
        }
        break;
    }
    case SAT_IN_DONE:
        g_loading = 0;
        g_sel = -1;
        for (int i = 0; i < g_n; i++) if (g_sats[i].e.norad == g_want) g_sel = i;
        if (g_sel < 0 && g_n) g_sel = 0;
        if (g_sel >= 0) g_want = g_sats[g_sel].e.norad;
        g_passFor = -1;
        break;
    case SAT_IN_PLACES: {
        int n = h.count;
        if (n > SAT_MAX_PLACES) n = SAT_MAX_PLACES;
        if ((uint32_t)n * sizeof(SatPlace) > h.bytes) n = (int)(h.bytes / sizeof(SatPlace));
        memcpy(g_places, p, (size_t)n * sizeof(SatPlace));
        for (int i = 0; i < n; i++) g_places[i].name[SAT_NAME_LEN - 1] = 0;
        g_placeCount = n;
        g_pickSel = 0;
        g_searching = 0;
        break;
    }
    case SAT_IN_STATUS:
        if (h.bytes >= 2) {
            int n = p[1];
            if (n > (int)h.bytes - 2) n = (int)h.bytes - 2;
            char buf[128];
            if (n > (int)sizeof buf - 1) n = sizeof buf - 1;
            memcpy(buf, p + 2, (size_t)n);
            buf[n] = 0;
            set_status(p[0], buf);
            if (p[0] == SAT_STATUS_ERROR) { g_searching = 0; g_loading = 0; }
        }
        break;
    default:
        break;
    }
}

// Set-1 scancodes.
#define SC_ESC 0x01
#define SC_BACK 0x0E
#define SC_TAB 0x0F
#define SC_ENTER 0x1C
#define SC_UP 0x48
#define SC_DOWN 0x50
#define SC_LEFT 0x4B
#define SC_RIGHT 0x4D
#define SC_HOME 0x47

static char scancode_char(int sc, int shift)
{
    static const char lower[0x40] =
        "\0\0" "1234567890-=" "\0\0" "qwertyuiop[]" "\0\0" "asdfghjkl;'`" "\0\\" "zxcvbnm,./" "\0\0\0 ";
    static const char upper[0x40] =
        "\0\0" "!@#$%^&*()_+" "\0\0" "QWERTYUIOP{}" "\0\0" "ASDFGHJKL:\"~" "\0|" "ZXCVBNM<>?" "\0\0\0 ";
    if (sc < 0 || sc >= 0x40) return 0;
    char c = shift ? upper[sc] : lower[sc];
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return c;
    if (c == ' ' || c == ',' || c == '.' || c == '-' || c == '\'') return c;
    return 0;
}

static void picker_key(int sc, int shift)
{
    switch (sc) {
    case SC_ESC: g_picker = 0; g_firstRun = 0; return;
    case SC_ENTER:
        if (g_placeCount > 0 && !strcmp(g_query, g_lastSearch)) do_pick(g_pickSel);
        else do_search();
        return;
    case SC_UP:   if (g_placeCount > 0 && g_pickSel > 0) g_pickSel--; return;
    case SC_DOWN: if (g_placeCount > 0 && g_pickSel < g_placeCount - 1 && g_pickSel < PICK_VISIBLE - 1) g_pickSel++; return;
    case SC_BACK: if (g_qlen > 0) g_query[--g_qlen] = 0; return;
    default: {
        char c = scancode_char(sc, shift);
        if (c && g_qlen < SAT_SEARCH_LEN && !(c == ' ' && g_qlen == 0)) {
            if (c >= 'a' && c <= 'z' && (g_qlen == 0 || g_query[g_qlen - 1] == ' ')) c = (char)(c - 32);
            g_query[g_qlen++] = c;
            g_query[g_qlen] = 0;
        }
    }
    }
}

void trace_on_input(int32_t type, int32_t flags, int32_t a, int32_t b, int32_t c)
{
    switch (type) {
    case 1: {   // TE_IN_KEY
        if (!(flags & 1)) break;
        int shift = (flags & 4) != 0;
        if (g_picker) { picker_key(a, shift); break; }
        switch (a) {
        case SC_ESC: g_quitting = 1; break;
        case SC_UP: case SC_LEFT:    if (g_n) select_sat(g_sel > 0 ? g_sel - 1 : g_n - 1); break;
        case SC_DOWN: case SC_RIGHT: case SC_TAB: if (g_n) select_sat(g_sel < g_n - 1 ? g_sel + 1 : 0); break;
        case SC_HOME: if (g_n) select_sat(0); break;
        default: {
            char ch = scancode_char(a, 0);
            if (ch >= '1' && ch < '1' + GRP_COUNT) set_group(ch - '1');
            else if (ch == 'l') open_picker();
            else if (ch == 'q') g_quitting = 1;
            else if (ch == 'i') {
                int found = 0;
                for (int i = 0; i < g_n; i++) if (g_sats[i].e.norad == SAT_ISS) { select_sat(i); found = 1; }
                if (!found) { g_want = SAT_ISS; set_group(GRP_STATIONS); }
            } else if (ch == 'p' && g_nPass) g_passRow = (g_passRow + 1) % g_nPass;
            break;
        }
        }
        break;
    }
    case TRACE_INPUT_MOUSE_POS:
        g_mx = mouse_lx(b);
        g_my = mouse_ly(c);
        g_mouseOver = flags & 1;
        if (g_picker) {
            int r = picker_row_at(g_mx, g_my);
            if (r >= 0) g_pickSel = r;
            g_hover = -1;
        } else if (g_my >= MAP_Y + 30 && g_my < MAP_Y + MAP_H) {
            g_hover = sat_at(g_mx, g_my);
        } else {
            g_hover = -1;
        }
        break;
    case TRACE_INPUT_MOUSE_BUTTON: {
        int pressed = flags & 1, btn = a;
        if (btn == 4 || btn == 5) {
            if (g_picker && g_placeCount > 0) {
                g_pickSel += btn == 4 ? -1 : 1;
                if (g_pickSel < 0) g_pickSel = 0;
                if (g_pickSel >= g_placeCount) g_pickSel = g_placeCount - 1;
            } else if (!g_picker && g_n) {
                select_sat(btn == 4 ? (g_sel > 0 ? g_sel - 1 : g_n - 1) : (g_sel < g_n - 1 ? g_sel + 1 : 0));
            }
            break;
        }
        if (!pressed || btn != 1 || !g_mouseOver) break;
        if (g_picker) {
            int r = picker_row_at(g_mx, g_my);
            if (r >= 0) do_pick(r);
            else if (!inside(R_PICKER, g_mx, g_my)) { g_picker = 0; g_firstRun = 0; }
            break;
        }
        if (inside(R_CLOSE, g_mx, g_my)) { g_quitting = 1; break; }
        if (inside(g_placeBtn, g_mx, g_my)) { open_picker(); break; }
        for (int g = 0; g < GRP_COUNT; g++) if (inside(g_chip[g], g_mx, g_my)) { set_group(g); return; }
        for (int i = 0; i < g_nPass; i++) if (inside(pass_row(i), g_mx, g_my)) { g_passRow = i; return; }
        if (g_hover >= 0) select_sat(g_hover);
        break;
    }
    case 6:
        g_quitting = 1;
        break;
    default:
        break;
    }
}

void trace_update(void)
{
    if (g_quitting) trace_quit(0);
    if (g_wantS != g_S) { set_scale(g_wantS); g_cacheAt = -100000; }
    if (!g_frame) return;
    if (g_sel >= 0 && (g_passFor != g_sel || trace_time_ms() - g_passAt > 300000)) refresh_passes();

    clip_all();
    fill_d(0, 0, g_W, g_H, C_BG, 255);
    draw_map_base();
    clip_l(MAP_X, MAP_Y, MAP_W, MAP_H);
    draw_satellites();
    clip_all();
    draw_chips();
    draw_status();
    draw_info();
    draw_passes();
    draw_sky();
    draw_bar();
    if (g_picker) draw_picker();
    present_fitted(TRACE_PRESENT_ASPECT_4_3);
}
