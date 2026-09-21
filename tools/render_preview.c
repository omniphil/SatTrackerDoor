// Compiles the TRACE module natively with stub host functions and plays the
// door's part with the door's own code (sats.c: real CelesTrak orbits) and the
// real Earth asset (data/earth.bin). Writes a frame to a .ppm, so the picture
// can be checked without Windows, TERMinator or a BBS.
//
//   make -C door preview && door/preview [options] out.ppm
//     --group N       group 0..5 (stations, brightest, weather, ham, gps, science)
//     --norad N       follow this satellite (default the ISS)
//     --at UNIX       pretend it's this moment (default now)
//     --window W H    the window size TERMinator reports (default 1400x1050: 2x)
//     --mouse X Y     the pointer at logical X,Y (hover)
//     --picker        the place picker open, with sample results
//     --t SECONDS     run the animation this long first (default 1)
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../module/src/satmodule.c"
#include "../door/sats.h"

static uint32_t *g_shot;
static int g_shotW, g_shotH;
static int32_t g_clock = 0;
static uint8_t *g_asset;
static int32_t g_assetSize;
static int g_ready = 0;

void trace_present(const uint32_t *pixels, int32_t w, int32_t h, int32_t flags)
{
    (void)flags;
    free(g_shot);
    g_shot = malloc((size_t)w * h * 4);
    memcpy(g_shot, pixels, (size_t)w * h * 4);
    g_shotW = w; g_shotH = h;
}
int32_t trace_input_pending(void) { return 0; }
int32_t trace_cpu_count(void) { return 1; }
void    trace_frame_capacity(int32_t *w, int32_t *h) { *w = 3840; *h = 2400; }
void    trace_log(const char *t, int32_t n) { fwrite(t, 1, (size_t)n, stderr); fputc('\n', stderr); }
void    trace_quit(int32_t code) { (void)code; }
int32_t trace_time_ms(void) { return g_clock; }
void    trace_set_tick(int32_t hz) { (void)hz; }
void    trace_text_input(int32_t on) { (void)on; }
void    trace_mouse_mode(int32_t mode) { (void)mode; }
void    trace_pad_rumble(int32_t a, int32_t b, int32_t c, int32_t d) { (void)a;(void)b;(void)c;(void)d; }
int32_t trace_send_room(void) { return 4096; }
int32_t trace_audio_write(const int16_t *f, int32_t n) { (void)f; return n; }
int32_t trace_audio_room(void) { return 4096; }
int32_t trace_store_read(void *b, int32_t n) { (void)b; (void)n; return 0; }
int32_t trace_store_write(const void *d, int32_t n) { (void)d; (void)n; return 0; }
int32_t trace_asset_size(const char *sha) { (void)sha; return g_assetSize; }
int32_t trace_asset_read(const char *sha, int32_t off, void *buf, int32_t len)
{
    (void)sha;
    if (off >= g_assetSize) return 0;
    if (off + len > g_assetSize) len = g_assetSize - off;
    memcpy(buf, g_asset + off, (size_t)len);
    return len;
}
int32_t trace_send(const void *d, int32_t n)
{
    SatMsgHeader h;
    memcpy(&h, d, sizeof h);
    if (h.type == SAT_OUT_READY) g_ready = 1;
    return n;
}

static void to_module(uint8_t type, uint8_t flags, uint16_t count, const void *payload, size_t len)
{
    size_t total = 4 + sizeof(SatMsgHeader) + len;
    uint8_t *buf = malloc(total);
    memcpy(buf, "msg\n", 4);
    SatMsgHeader h = { type, flags, count, (uint32_t)len };
    memcpy(buf + 4, &h, sizeof h);
    if (payload && len) memcpy(buf + 4 + sizeof h, payload, len);
    trace_on_data((const char *)buf, (int32_t)total);
    free(buf);
}

int main(int argc, char **argv)
{
    int group = 0, winW = 1400, winH = 1050, picker = 0;
    uint32_t norad = SAT_ISS;
    double at = (double)time(NULL);
    float mouseX = -1, mouseY = -1, t = 1;
    const char *out = "preview.ppm";
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--group")) group = atoi(argv[++i]);
        else if (!strcmp(a, "--norad")) norad = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(a, "--at")) at = atof(argv[++i]);
        else if (!strcmp(a, "--window")) { winW = atoi(argv[++i]); winH = atoi(argv[++i]); }
        else if (!strcmp(a, "--mouse")) { mouseX = (float)atof(argv[++i]); mouseY = (float)atof(argv[++i]); }
        else if (!strcmp(a, "--picker")) picker = 1;
        else if (!strcmp(a, "--t")) t = (float)atof(argv[++i]);
        else out = a;
    }
    FILE *f = fopen("../data/earth.bin", "rb");
    if (!f) { fprintf(stderr, "run from door/: needs ../data/earth.bin\n"); return 1; }
    fseek(f, 0, SEEK_END); g_assetSize = (int32_t)ftell(f); fseek(f, 0, SEEK_SET);
    g_asset = malloc((size_t)g_assetSize);
    if (fread(g_asset, 1, (size_t)g_assetSize, f) != (size_t)g_assetSize) return 1;
    fclose(f);

    sat_load_config();
    trace_init();
    trace_on_resize(winW, winH);
    char hash[80];
    snprintf(hash, sizeof hash, "earth=%064d", 0);
    trace_on_data(hash, (int32_t)strlen(hash));
    trace_on_data("start", 5);
    if (g_ready) {
        SatPrefs p;
        memset(&p, 0, sizeof p);
        p.doorNow = (uint32_t)at;
        int off = -25200;
        sat_utc_offset(45.5235, -122.6762, &off);
        p.utcOffset = off;
        p.lat1e4 = 455235; p.lon1e4 = -1226762;
        p.selected = norad;
        p.group = (uint8_t)group;
        strcpy(p.place, "Portland, Oregon");
        to_module(SAT_IN_PREFS, 0, 1, &p, sizeof p);
        static OrbElements e[SAT_MAX];
        char err[200];
        int n = sat_group(group, e, SAT_MAX, err, sizeof err);
        SatWire w[42];
        int k = 0, first = 1;
        for (int i = 0; i <= n; i++) {
            if (i == n || k == 42) { to_module(SAT_IN_ELEMS, first, (uint16_t)k, w, (size_t)k * sizeof(SatWire)); first = 0; k = 0; if (i == n) break; }
            SatWire *s = &w[k++];
            memset(s, 0, sizeof *s);
            s->epoch = e[i].epoch; s->incl = e[i].incl; s->raan = e[i].raan; s->ecc = e[i].ecc;
            s->argp = e[i].argp; s->mo = e[i].mo; s->nRevDay = e[i].nRevDay; s->bstar = e[i].bstar;
            s->norad = e[i].norad; memcpy(s->name, e[i].name, 24);
        }
        uint8_t g = (uint8_t)group;
        to_module(SAT_IN_DONE, 0, 1, &g, 1);
    }
    int frames = (int)(t * 30);
    for (int i = 0; i <= frames; i++) {
        g_clock = 10000 + i * 33;
        if (i == 1 && mouseX >= 0) trace_on_input(TRACE_INPUT_MOUSE_POS, 1, 0, (int32_t)(mouseX * g_presW / LW), (int32_t)(mouseY * g_presH / LH));
        if (i == frames && picker) {
            open_picker();
            strcpy(g_query, "Springfield"); g_qlen = 11; strcpy(g_lastSearch, g_query);
            const char *names[] = { "Springfield, Illinois", "Springfield, Missouri", "Springfield, Massachusetts" };
            for (int k = 0; k < 3; k++) { memset(&g_places[k], 0, sizeof g_places[k]); strcpy(g_places[k].name, names[k]); }
            g_placeCount = 3;
        }
        trace_update();
    }
    if (!g_shot) { fprintf(stderr, "module never presented a frame\n"); return 1; }
    FILE *o = fopen(out, "wb");
    fprintf(o, "P6\n%d %d\n255\n", g_shotW, g_shotH);
    for (int i = 0; i < g_shotW * g_shotH; i++) {
        uint32_t px = g_shot[i];
        uint8_t rgb[3] = { (uint8_t)(px >> 16), (uint8_t)(px >> 8), (uint8_t)px };
        fwrite(rgb, 1, 3, o);
    }
    fclose(o);
    fprintf(stderr, "wrote %s (%dx%d)\n", out, g_shotW, g_shotH);
    return 0;
}
