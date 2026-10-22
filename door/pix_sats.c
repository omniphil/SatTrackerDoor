/*
 * pix_sats.c -- the satellite tracker as JPEG XL pictures: the mode between TRACE and ANSI.
 *
 * The TRACE module (module/src/satmodule.c) is compiled in here and runs on the BBS, as tools/render_preview.c runs
 * it, so the caller sees exactly the TRACE map and panels without TERMinator. Its frames go out as JPEG XL through
 * the CTerm APC picture commands, the same way the News door's JPEG XL mode (and the DOOM door's) sends them:
 *
 *   Pictures   The module draws at 640x480, its 1x size (its 2x, 1280x960, costs four times the drawing on the BBS
 *              and shrunk to 640 across looks the same), and the frame is shrunk to the caller's screen (640x400 on
 *              nearly every CTerm terminal, which shows it at 4:3, as the module means it to be seen). Only what changed goes:
 *              the screen is cut into 32x16 tiles and each run of changed tiles is its own small picture, so the
 *              moving satellites, the pulsing marker and the clocks cost a few tiles, not a screen.
 *   Pacing     A cursor-position request follows every frame, and its answer says the frame has arrived; as many
 *              frames may be on their way as the link's speed times its round trip allows (pix_play.c in the DOOM
 *              door explains it). At most TARGET_FPS frames a second, and IDLE_FPS once the caller has left the
 *              keys and mouse alone for a while: the satellites creep a pixel every few seconds, so nothing is lost.
 *   Quality    JPEG XL's distance follows the link, and tiles that settle are sent once more, sharp, while it idles.
 *   Input      Keys as the ANSI view reads them, turned into the scancodes the module takes; the mouse through xterm
 *              SGR reporting (cells, aimed at the cell's middle).
 *   The door   Orbits, place searches and time zones are fetched on a worker thread (they can take seconds), and the
 *              packets it makes are handed to the module between frames, so the picture never stops for the network.
 *              The Earth pictures (earth.bin) are read from beside the door, where TRACE sends them from.
 *
 * Set SATDOOR_LOG=<file> for frame rate, bandwidth, quality and round trip every 5 seconds; otherwise the last
 * session's go to saves/<user>/jxl.log.
 */
#define _GNU_SOURCE

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <sys/select.h>
#include <time.h>
#include <unistd.h>

#include "satmodule.c"      /* from $(MODSRC): ../module/src, or native/ on the BBS */

#include "apc.h"
#include "door.h"
#include "jxl_enc.h"
#include "pix_sats.h"
#include "sats.h"

/* The scancodes the module doesn't name itself (set 1) */
#define SC_SPACE 0x39
#define SC_F1    0x3B
#define SC_END   0x4F
#define SC_PGUP  0x49
#define SC_PGDN  0x51

#define RW 640                      /* what the module draws at */
#define RH 480
#define TW 32
#define TH 16
#define MAX_PX_W 1920
#define MAX_PX_H 1200

#define MAX_IN_FLIGHT    16
#define MIN_WINDOW       3
#define QUEUE_ALLOWED_MS 50
#define STALL_MS         3000
#define FULL_FRACTION    0.6
#define SETTLE_FRAMES    6
#define SHARP_DISTANCE   0.4f
#define TARGET_FPS       20
#define ESC_MS           400        /* a lone ESC this long is the Esc key */
#define CACHE_FPS        10         /* terminals that take pictures only through their cache: a file write each */
#define IDLE_FPS         4          /* after IDLE_AFTER_MS without a key or the mouse */
#define IDLE_AFTER_MS    3000
#define NOTICE           48
#define TIDY_FRAMES      40         /* frames before a tile slightly off what was sent is sent anyway */         /* levels a pixel may drift from what was sent before its tile goes again */
#define MIN_DISTANCE     0.4f
#define MAX_DISTANCE     6.0f
#define START_DISTANCE   1.5f
#define CONTROL_MS       250

static long px_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

static bool px_write_all(const char *data, size_t len)
{
    while (len > 0) {
        ssize_t n = write(STDOUT_FILENO, data, len);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN) {
                struct timeval tv = { 0, 10000 };
                fd_set fds;
                FD_ZERO(&fds);
                FD_SET(STDOUT_FILENO, &fds);
                select(STDOUT_FILENO + 1, NULL, &fds, NULL, &tv);
                continue;
            }
            return false;
        }
        data += n;
        len -= (size_t)n;
    }
    return true;
}

/* ------------------------------------------------- the module's host --- */

static long     px_start;
static bool     px_quit;
static uint32_t *px_frame;          /* the module's last frame (BGRA) */
static int      px_frameW, px_frameH;
static unsigned px_frameSeq;

void trace_present(const uint32_t *pixels, int32_t w, int32_t h, int32_t flags)
{
    (void)flags;
    if (w != px_frameW || h != px_frameH) {
        free(px_frame);
        px_frame = malloc((size_t)w * h * 4);
        if (!px_frame) { px_frameW = px_frameH = 0; return; }
        px_frameW = w;
        px_frameH = h;
    }
    memcpy(px_frame, pixels, (size_t)w * h * 4);
    px_frameSeq++;
}
int32_t trace_input_pending(void) { return 0; }
int32_t trace_cpu_count(void) { return 1; }
void    trace_frame_capacity(int32_t *w, int32_t *h) { *w = RW; *h = RH; }
void    trace_log(const char *t, int32_t n) { (void)t; (void)n; }
void    trace_quit(int32_t code) { (void)code; px_quit = true; }
int32_t trace_time_ms(void) { return (int32_t)(px_now() - px_start); }
static int px_tickMs = 33;          /* the module's own rate (it asks for 30 Hz) */
void    trace_set_tick(int32_t hz) { px_tickMs = hz > 0 ? 1000 / hz : 33; }
void    trace_text_input(int32_t on) { (void)on; }
void    trace_mouse_mode(int32_t mode) { (void)mode; }
void    trace_pad_rumble(int32_t a, int32_t b, int32_t c, int32_t d) { (void)a; (void)b; (void)c; (void)d; }
int32_t trace_send_room(void) { return TRACE_SEND_MAX; }
int32_t trace_audio_write(const int16_t *f, int32_t n) { (void)f; return n; }
int32_t trace_audio_room(void) { return 4096; }
int32_t trace_store_read(void *b, int32_t n) { (void)b; (void)n; return 0; }
int32_t trace_store_write(const void *d, int32_t n) { (void)d; (void)n; return 0; }

/* The one asset is the Earth pictures, read from earth.bin beside the door. */
static uint8_t *px_earth;
static int32_t  px_earthSize;

int32_t trace_asset_size(const char *sha) { (void)sha; return px_earthSize; }
int32_t trace_asset_read(const char *sha, int32_t off, void *buf, int32_t len)
{
    (void)sha;
    if (off < 0 || off >= px_earthSize) return 0;
    if (len > px_earthSize - off) len = px_earthSize - off;
    memcpy(buf, px_earth + off, (size_t)len);
    return len;
}

static void load_earth_file(void)
{
    char path[1100];
    FILE *f = fopen(sat_beside_exe("earth.bin", path, sizeof path), "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    px_earth = size > 0 ? malloc((size_t)size) : NULL;
    if (px_earth && fread(px_earth, 1, (size_t)size, f) == (size_t)size) px_earthSize = (int32_t)size;
    fclose(f);
}

/* ------------------------------------------- the door, on its own thread ---
 *
 * Jobs in (from the module's requests), packets out (for the module). Only the worker touches the caller's prefs and
 * the place list; the main thread only passes packets along. The work is main.c's TRACE link, done the same way.
 */

enum { JOB_READY = 1, JOB_GROUP, JOB_SEARCH, JOB_PICK, JOB_SELECT };

#define JMAX 32
typedef struct { uint8_t kind; uint32_t arg; char query[SAT_SEARCH_LEN + 1]; } Job;
static Job  px_jobs[JMAX];
static int  px_jobN;
static bool px_stop;
static pthread_mutex_t px_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  px_wake = PTHREAD_COND_INITIALIZER;

typedef struct Packet { struct Packet *next; size_t len; uint8_t data[]; } Packet;
static Packet *px_head, *px_tail;

static SatUserPrefs *px_prefs;
static SatPlace px_places[SAT_MAX_PLACES];
static int      px_placeCount;

/* A newer group, search or selection replaces one still waiting: only the last one matters. */
static void job_add(uint8_t kind, uint32_t arg, const char *query)
{
    pthread_mutex_lock(&px_lock);
    int at = px_jobN;
    if (kind != JOB_PICK)
        for (int i = 0; i < px_jobN; i++) if (px_jobs[i].kind == kind) at = i;
    if (at < JMAX) {
        px_jobs[at].kind = kind;
        px_jobs[at].arg = arg;
        snprintf(px_jobs[at].query, sizeof px_jobs[at].query, "%s", query ? query : "");
        if (at == px_jobN) px_jobN++;
        pthread_cond_signal(&px_wake);
    }
    pthread_mutex_unlock(&px_lock);
}

/* What the worker makes, queued as the module will be handed it ("msg\n", header, payload). */
static void to_module_queue(uint8_t type, uint8_t flags, uint16_t count, const void *payload, size_t len)
{
    Packet *p = malloc(sizeof(Packet) + 4 + sizeof(SatMsgHeader) + len);
    if (!p) return;
    SatMsgHeader h = { type, flags, count, (uint32_t)len };
    memcpy(p->data, "msg\n", 4);
    memcpy(p->data + 4, &h, sizeof h);
    if (payload && len) memcpy(p->data + 4 + sizeof h, payload, len);
    p->len = 4 + sizeof h + len;
    p->next = NULL;
    pthread_mutex_lock(&px_lock);
    if (px_tail) px_tail->next = p; else px_head = p;
    px_tail = p;
    pthread_mutex_unlock(&px_lock);
}

static void q_status(int kind, const char *text)
{
    unsigned char buf[2 + 120];
    size_t n = strlen(text);
    if (n > 120) n = 120;
    buf[0] = (unsigned char)kind;
    buf[1] = (unsigned char)n;
    memcpy(buf + 2, text, n);
    to_module_queue(SAT_IN_STATUS, 0, 1, buf, 2 + n);
}

static void q_prefs(void)
{
    SatPrefs p;
    memset(&p, 0, sizeof p);
    p.doorNow = (uint32_t)time(NULL);
    p.utcOffset = px_prefs->utcOffset > -900000 ? px_prefs->utcOffset : 0;
    p.lat1e4 = (int32_t)(px_prefs->lat * 1e4);
    p.lon1e4 = (int32_t)(px_prefs->lon * 1e4);
    p.selected = px_prefs->selected;
    p.group = (uint8_t)px_prefs->group;
    p.firstRun = px_prefs->havePlace ? 0 : 1;
    memcpy(p.place, px_prefs->place, SAT_NAME_LEN);
    to_module_queue(SAT_IN_PREFS, 0, 1, &p, sizeof p);
}

static void q_group(int group)
{
    static OrbElements e[SAT_MAX];
    char err[200];
    q_status(SAT_STATUS_BUSY, "Fetching orbits...");
    int n = sat_group(group, e, SAT_MAX, err, sizeof err);
    if (n < 0) { q_status(SAT_STATUS_ERROR, err); return; }
    SatWire w[42];
    int k = 0, first = 1;
    for (int i = 0; i <= n; i++) {
        if (i == n || k == 42) {
            to_module_queue(SAT_IN_ELEMS, first ? 1 : 0, (uint16_t)k, w, (size_t)k * sizeof(SatWire));
            first = 0;
            k = 0;
            if (i == n) break;
        }
        SatWire *s = &w[k++];
        memset(s, 0, sizeof *s);
        s->epoch = e[i].epoch;
        s->incl = e[i].incl;
        s->raan = e[i].raan;
        s->ecc = e[i].ecc;
        s->argp = e[i].argp;
        s->mo = e[i].mo;
        s->nRevDay = e[i].nRevDay;
        s->bstar = e[i].bstar;
        s->norad = e[i].norad;
        memcpy(s->name, e[i].name, sizeof s->name);
    }
    uint8_t g = (uint8_t)group;
    to_module_queue(SAT_IN_DONE, 0, 1, &g, 1);
    char msg[80];
    snprintf(msg, sizeof msg, "%d satellites in %s", n, SAT_GROUP_NAME[group]);
    q_status(SAT_STATUS_INFO, msg);
}

static void *worker(void *arg)
{
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&px_lock);
        while (!px_stop && px_jobN == 0) pthread_cond_wait(&px_wake, &px_lock);
        if (px_stop) { pthread_mutex_unlock(&px_lock); break; }
        Job j = px_jobs[0];
        memmove(&px_jobs[0], &px_jobs[1], (size_t)(px_jobN - 1) * sizeof px_jobs[0]);
        px_jobN--;
        pthread_mutex_unlock(&px_lock);

        int off;
        switch (j.kind) {
        case JOB_READY:
            if (px_prefs->havePlace || px_prefs->utcOffset < -900000) {
                if (sat_utc_offset(px_prefs->lat, px_prefs->lon, &off)) {
                    px_prefs->utcOffset = off;
                    sat_save_prefs(px_prefs);
                }
            }
            q_prefs();
            q_group(px_prefs->group);
            break;
        case JOB_GROUP:
            if (j.arg < GRP_COUNT) {
                px_prefs->group = (int)j.arg;
                sat_save_prefs(px_prefs);
                q_group(px_prefs->group);
            }
            break;
        case JOB_SEARCH: {
            int n = sat_search(j.query, px_places, SAT_MAX_PLACES);
            if (n < 0) { px_placeCount = 0; q_status(SAT_STATUS_ERROR, "Couldn't reach the place search. Try again."); }
            else {
                px_placeCount = n;
                to_module_queue(SAT_IN_PLACES, 0, (uint16_t)n, px_places, (size_t)n * sizeof(SatPlace));
            }
            break;
        }
        case JOB_PICK:
            if ((int)j.arg < px_placeCount) {
                const SatPlace *p = &px_places[j.arg];
                memcpy(px_prefs->place, p->name, SAT_NAME_LEN);
                px_prefs->lat = p->lat1e4 / 1e4;
                px_prefs->lon = p->lon1e4 / 1e4;
                px_prefs->havePlace = true;
                if (sat_utc_offset(px_prefs->lat, px_prefs->lon, &off)) px_prefs->utcOffset = off;
                sat_save_prefs(px_prefs);
                q_prefs();
            }
            break;
        case JOB_SELECT:
            px_prefs->selected = j.arg;
            sat_save_prefs(px_prefs);
            break;
        }
    }
    return NULL;
}

/* The worker's packets, into the module. */
static void deliver(void)
{
    pthread_mutex_lock(&px_lock);
    Packet *p = px_head;
    px_head = px_tail = NULL;
    pthread_mutex_unlock(&px_lock);
    while (p) {
        Packet *next = p->next;
        trace_on_data((const char *)p->data, (int32_t)p->len);
        free(p);
        p = next;
    }
}

/* The module talking to the door. */
int32_t trace_send(const void *d, int32_t n)
{
    SatMsgHeader h;
    if (n < (int32_t)sizeof h) return n;
    memcpy(&h, d, sizeof h);
    const uint8_t *body = (const uint8_t *)d + sizeof h;
    if ((uint32_t)(n - (int32_t)sizeof h) < h.bytes) return n;
    switch (h.type) {
    case SAT_OUT_READY: job_add(JOB_READY, 0, NULL); break;
    case SAT_OUT_GROUP: if (h.bytes >= 1) job_add(JOB_GROUP, body[0], NULL); break;
    case SAT_OUT_PICK:  if (h.bytes >= 1) job_add(JOB_PICK, body[0], NULL); break;
    case SAT_OUT_SEARCH:
        if (h.bytes >= 1) {
            char q[SAT_SEARCH_LEN + 1];
            size_t len = body[0];
            if (len > h.bytes - 1) len = h.bytes - 1;
            if (len > SAT_SEARCH_LEN) len = SAT_SEARCH_LEN;
            memcpy(q, body + 1, len);
            q[len] = 0;
            job_add(JOB_SEARCH, 0, q);
        }
        break;
    case SAT_OUT_SELECT:
        if (h.bytes >= 4) {
            uint32_t norad;
            memcpy(&norad, body, 4);
            job_add(JOB_SELECT, norad, NULL);
        }
        break;
    default:
        break;
    }
    return n;
}

/* ------------------------------------------------------------ the link --- */

static struct {
    long   sent[MAX_IN_FLIGHT];
    size_t bytes[MAX_IN_FLIGHT];
    size_t total;
    int    head, count;
} px_fly;
static long   px_rttSum, px_rttN, px_rttFloor[10], px_rttFloorSec = -1;
static size_t px_acked;
static double px_rate, px_frameAvg = 12000;
static int    px_orphans;

static void fly_push(long now, size_t bytes)
{
    int i = (px_fly.head + px_fly.count) % MAX_IN_FLIGHT;
    px_fly.sent[i] = now;
    px_fly.bytes[i] = bytes;
    px_fly.total += bytes;
    px_fly.count++;
}

static void fly_reset(void)
{
    px_orphans += px_fly.count;
    px_fly.count = 0;
    px_fly.total = 0;
}

static long rtt_min(void)
{
    long m = 0;
    for (int i = 0; i < 10; i++)
        if (px_rttFloor[i] > 0 && (m == 0 || px_rttFloor[i] < m)) m = px_rttFloor[i];
    return m;
}

static void fly_ack(long now)
{
    long sec = now / 1000;
    if (px_orphans > 0) { px_orphans--; return; }
    if (px_fly.count == 0) return;
    long rtt = now - px_fly.sent[px_fly.head];
    px_acked += px_fly.bytes[px_fly.head];
    px_fly.total -= px_fly.bytes[px_fly.head];
    px_fly.head = (px_fly.head + 1) % MAX_IN_FLIGHT;
    px_fly.count--;
    px_rttSum += rtt;
    px_rttN++;
    if (sec != px_rttFloorSec) { px_rttFloorSec = sec; px_rttFloor[sec % 10] = rtt; }
    else if (rtt < px_rttFloor[sec % 10]) px_rttFloor[sec % 10] = rtt;
}

static bool window_open(void)
{
    double window = px_rate * (double)(rtt_min() + QUEUE_ALLOWED_MS);
    if (window < MIN_WINDOW * px_frameAvg) window = MIN_WINDOW * px_frameAvg;
    return px_fly.count < MAX_IN_FLIGHT && (double)px_fly.total < window;
}

/* ------------------------------------------------------------- input --- */

static int  px_cellW = 8, px_cellH = 16, px_W, px_H;
static long px_inputAt;             /* the last key or mouse report */

static void key(int sc, bool shift)
{
    int flags = 1 | (shift ? 4 : 0);
    px_inputAt = px_now();
    trace_on_input(1, flags, sc, 0, 0);
    trace_on_input(1, flags & ~1, sc, 0, 0);
}

/* A typed character as the scancode the module reads (letters, digits and what a place name needs). */
static void typed(unsigned char c)
{
    static const char *const rows[] = { "1234567890-", "qwertyuiop", "asdfghjkl;'", "zxcvbnm,." };
    static const int first[] = { 0x02, 0x10, 0x1E, 0x2C };
    static bool afterCR;
    bool lf = c == '\n' && afterCR;        /* CR LF (or CR NUL) is one Enter */
    afterCR = c == '\r';
    if (lf || c == 0) return;
    if (c == '\r' || c == '\n') { key(SC_ENTER, false); return; }
    if (c == 27) { key(SC_ESC, false); return; }
    if (c == '\t') { key(SC_TAB, false); return; }
    if (c == 8 || c == 127) { key(SC_BACK, false); return; }
    if (c == ' ') { key(SC_SPACE, false); return; }
    bool shift = c >= 'A' && c <= 'Z';
    if (shift) c = (unsigned char)(c - 'A' + 'a');
    for (int r = 0; r < 4; r++) {
        const char *p = strchr(rows[r], c);
        if (p && c) { key(first[r] + (int)(p - rows[r]), shift); return; }
    }
}

/* The mouse at a cell, in the module's frame pixels: the cell's middle. */
static void mouse_at(int col, int row, int buttons)
{
    int x = (col - 1) * px_cellW + px_cellW / 2, y = (row - 1) * px_cellH + px_cellH / 2;
    px_inputAt = px_now();
    if (x >= px_W) x = px_W - 1;
    if (y >= px_H) y = px_H - 1;
    trace_on_input(TRACE_INPUT_MOUSE_POS, 1, buttons, x * RW / px_W, y * RH / px_H);
}

static int px_buttons;

static void sgr_mouse(const char *params, bool press)
{
    int b, col, row;
    if (sscanf(params, "%d;%d;%d", &b, &col, &row) != 3) return;
    bool motion = (b & 32) != 0;
    int base = b & ~(4 | 8 | 16 | 32);
    if (base == 64 || base == 65) {                 /* the wheel */
        mouse_at(col, row, px_buttons);
        if (press) {
            trace_on_input(TRACE_INPUT_MOUSE_BUTTON, 1, base == 64 ? 4 : 5, 0, 0);
            trace_on_input(TRACE_INPUT_MOUSE_BUTTON, 0, base == 64 ? 4 : 5, 0, 0);
        }
        return;
    }
    int btn = base == 0 ? 1 : base == 1 ? 2 : base == 2 ? 3 : 0;
    if (!motion && btn) {
        int bit = btn == 1 ? 1 : btn == 2 ? 2 : 4;
        px_buttons = press ? (px_buttons | bit) : (px_buttons & ~bit);
    }
    mouse_at(col, row, px_buttons);
    if (!motion && btn) trace_on_input(TRACE_INPUT_MOUSE_BUTTON, press ? 1 : 0, btn, 0, 0);
}

static unsigned char px_seq[64];
static int  px_seqLen;
static long px_seqAt;

static void finish_sequence(long now)
{
    unsigned char final = px_seq[px_seqLen - 1];
    char params[64];
    bool ss3 = px_seq[1] == 'O';
    int n = px_seqLen - 3;
    memcpy(params, px_seq + 2, (size_t)n);
    params[n] = '\0';
    px_seqLen = 0;

    if (ss3) {
        switch (final) {
        case 'P': key(SC_F1, false); break;
        case 'Q': key(SC_F1 + 1, false); break;
        case 'R': key(SC_F1 + 2, false); break;
        case 'S': key(SC_F1 + 3, false); break;
        case 'A': key(SC_UP, false); break;
        case 'B': key(SC_DOWN, false); break;
        case 'C': key(SC_RIGHT, false); break;
        case 'D': key(SC_LEFT, false); break;
        case 'H': key(SC_HOME, false); break;
        case 'F': key(SC_END, false); break;
        }
        return;
    }
    if (params[0] == '<' && (final == 'M' || final == 'm')) { sgr_mouse(params + 1, final == 'M'); return; }
    if (final == 'R' && params[0] >= '0' && params[0] <= '9') { fly_ack(now); return; }
    if (params[0] == '=' || params[0] == '?' || params[0] == '<') return;     /* other answers */
    switch (final) {
    case 'A': key(SC_UP, false); break;
    case 'B': key(SC_DOWN, false); break;
    case 'C': key(SC_RIGHT, false); break;
    case 'D': key(SC_LEFT, false); break;
    case 'H': key(SC_HOME, false); break;
    case 'F': case 'K': key(SC_END, false); break;
    case 'V': key(SC_PGUP, false); break;           /* SyncTERM's own PgUp/PgDn */
    case 'U': key(SC_PGDN, false); break;
    case 'Z': key(SC_TAB, true); break;             /* Shift-Tab */
    case '~':
        switch (atoi(params)) {
        case 1: case 7: key(SC_HOME, false); break;
        case 4: case 8: key(SC_END, false); break;
        case 5: key(SC_PGUP, false); break;
        case 6: key(SC_PGDN, false); break;
        case 11: case 12: case 13: case 14: key(SC_F1 + atoi(params) - 11, false); break;
        }
        break;
    }
}

/* The older mouse reports (X10: ESC [ M then three bytes, each 32 more than the button, column and row), from
 * terminals without SGR reporting: passed on as SGR. A release doesn't say which button, so it's the last one pressed. */
static int px_x10Btn;

static void x10_mouse(int b, int col, int row)
{
    char params[48];
    bool press = true;
    if (!(b & 64) && (b & 3) == 3 && !(b & 32)) {   /* a release */
        b = (b & ~3) | px_x10Btn;
        press = false;
    } else if (!(b & 64) && !(b & 32)) {
        px_x10Btn = b & 3;
    }
    snprintf(params, sizeof params, "%d;%d;%d", b, col, row);
    sgr_mouse(params, press);
}

static void take_input(const unsigned char *data, int len, long now)
{
    for (int i = 0; i < len; i++) {
        unsigned char c = data[i];
        if (px_seqLen == 1) {
            if (c == '[' || c == 'O') { px_seq[px_seqLen++] = c; continue; }
            key(SC_ESC, false);                     /* ESC and something else: Escape, then that */
            px_seqLen = 0;
        } else if (px_seqLen >= 2) {
            if (px_seq[1] == '[' && (px_seqLen > 2 ? px_seq[2] == 'M' : c == 'M')) {
                /* X10 mouse: the three bytes after ESC [ M are raw, not part of a sequence */
                px_seq[px_seqLen++] = c;
                if (px_seqLen == 6) {
                    x10_mouse(px_seq[3] - 32, px_seq[4] - 32, px_seq[5] - 32);
                    px_seqLen = 0;
                }
                continue;
            }
            if (px_seqLen < (int)sizeof px_seq) px_seq[px_seqLen++] = c;
            if (c >= 0x40 && c <= 0x7E && !(px_seqLen == 3 && px_seq[1] == '[' && c == '[')) finish_sequence(now);
            else if (px_seqLen == (int)sizeof px_seq) px_seqLen = 0;
            continue;
        }
        if (c == 27) { px_seq[0] = c; px_seqLen = 1; px_seqAt = now; continue; }
        typed(c);
    }
    /* A lone ESC is the Esc key -- after ESC_MS, not sooner: a terminal that has fallen behind can send its answers
     * in pieces, and an answer's ESC taken for the key quit the door and typed the rest into the BBS (SyncTERM 1.8b
     * over SSH, 2026-10-05) */
    if (px_seqLen == 1 && now - px_seqAt > ESC_MS) { key(SC_ESC, false); px_seqLen = 0; }
}

/* ----------------------------------------------------------- picture --- */

static uint8_t *px_rgb, *px_ref, *px_tmp;
static int      px_TX, px_TY;
static float   *px_tileD;
static int     *px_tileStill;
static bool     px_haveRef, px_blob;
static Span    *px_sx, *px_sy;

/* The module's frame, shrunk (area filter, gfx.h's spans) to the screen. */
static void to_screen(void)
{
    if (!px_sx || px_frameW != RW || px_frameH != RH) {
        free(px_sx); free(px_sy);
        px_sx = spans(px_frameW, px_W);
        px_sy = spans(px_frameH, px_H);
        free(px_tmp);
        px_tmp = malloc((size_t)px_W * px_frameH * 4);
    }
    uint32_t *tmp = (uint32_t *)px_tmp;
    for (int y = 0; y < px_frameH; y++) {
        const uint32_t *row = px_frame + (size_t)y * px_frameW;
        uint32_t *o = tmp + (size_t)y * px_W;
        for (int x = 0; x < px_W; x++) {
            const Span *s = &px_sx[x];
            uint32_t rb = 0, g = 0;
            for (int k = 0; k < s->count; k++) {
                uint32_t p = row[s->first + k], w = s->w[k];
                rb += (p & 0x00FF00FFu) * w;
                g  += (p & 0x0000FF00u) * w;
            }
            o[x] = ((rb >> 8) & 0x00FF00FFu) | ((g >> 8) & 0x0000FF00u);
        }
    }
    for (int y = 0; y < px_H; y++) {
        const Span *s = &px_sy[y];
        uint8_t *d = px_rgb + (size_t)y * px_W * 3;
        for (int x = 0; x < px_W; x++) {
            uint32_t rb = 0, g = 0;
            for (int k = 0; k < s->count; k++) {
                uint32_t p = tmp[(size_t)(s->first + k) * px_W + x], w = s->w[k];
                rb += (p & 0x00FF00FFu) * w;
                g  += (p & 0x0000FF00u) * w;
            }
            d[x * 3 + 0] = (uint8_t)(rb >> 24);
            d[x * 3 + 1] = (uint8_t)(g >> 16);
            d[x * 3 + 2] = (uint8_t)(rb >> 8);
        }
    }
}

#define TILE(a, tx, ty) (a)[(ty) * px_TX + (tx)]

/*
 * Whether a tile has changed enough to send, against what the caller last got for it (px_ref holds only what was
 * sent). The satellites move a fraction of a pixel a frame, so their anti-aliased dots shift a level or two each
 * time: counted as changes, every dot's tile would go out every frame. Measured against the last sent picture
 * instead, a small change waits until it adds up to NOTICE levels, so a dot is resent as it moves, not as it shimmers.
 */
static bool tile_changed(int tx, int ty)
{
    int x0 = tx * TW, w = (x0 + TW > px_W ? px_W - x0 : TW) * 3;
    for (int y = ty * TH; y < (ty + 1) * TH && y < px_H; y++) {
        size_t off = ((size_t)y * px_W + x0) * 3;
        const uint8_t *a = px_rgb + off, *b = px_ref + off;
        if (memcmp(a, b, (size_t)w) == 0) continue;
        for (int i = 0; i < w; i++)
            if (abs((int)a[i] - (int)b[i]) > NOTICE) return true;
    }
    return false;
}

/* Whether a tile differs at all from what was sent. */
static bool tile_differs(int tx, int ty)
{
    int x0 = tx * TW, w = (x0 + TW > px_W ? px_W - x0 : TW) * 3;
    for (int y = ty * TH; y < (ty + 1) * TH && y < px_H; y++) {
        size_t off = ((size_t)y * px_W + x0) * 3;
        if (memcmp(px_rgb + off, px_ref + off, (size_t)w) != 0) return true;
    }
    return false;
}

/* What the caller now has for a tile. */
static void tile_sent(int tx, int ty)
{
    int x0 = tx * TW, w = (x0 + TW > px_W ? px_W - x0 : TW) * 3;
    TILE(px_tileStill, tx, ty) = 0;
    for (int y = ty * TH; y < (ty + 1) * TH && y < px_H; y++) {
        size_t off = ((size_t)y * px_W + x0) * 3;
        memcpy(px_ref + off, px_rgb + off, (size_t)w);
    }
}

static void draw_rect(outbuf_t *o, int x, int y, int w, int h, float distance)
{
    if (x + w > px_W) w = px_W - x;
    if (y + h > px_H) h = px_H - y;
    const uint8_t *jxl;
    size_t n = jxl_enc_rgb(px_rgb + ((size_t)y * px_W + x) * 3, px_W * 3, w, h, distance, &jxl);
    if (n == 0) return;
    char head[96];
    if (px_blob) {
        snprintf(head, sizeof head, "C;DrawJXLBlob;DX=%d;DY=%d;", x, y);
        apc_blob(o, head, jxl, n);
    } else {
        apc_blob(o, "C;S;satdoor/frame.jxl;", jxl, n);
        apc_cmd(o, "C;DrawJXL;DX=%d;DY=%d;satdoor/frame.jxl", x, y);
    }
}

static bool draw_tiles(outbuf_t *o, bool *tiles, float distance)
{
    int count = 0;
    for (int i = 0; i < px_TX * px_TY; i++) count += tiles[i];
    if (count == 0) return false;
    for (int ty = 0; ty < px_TY; ty++)
        for (int tx = 0; tx < px_TX; tx++)
            if (TILE(tiles, tx, ty)) tile_sent(tx, ty);
    if (count > FULL_FRACTION * px_TX * px_TY) {
        memcpy(px_ref, px_rgb, (size_t)px_W * px_H * 3);
        memset(px_tileStill, 0, (size_t)px_TX * px_TY * sizeof *px_tileStill);
        draw_rect(o, 0, 0, px_W, px_H, distance);
        for (int i = 0; i < px_TX * px_TY; i++) px_tileD[i] = distance;
        return true;
    }
    for (int ty = 0; ty < px_TY; ty++)
        for (int tx = 0; tx < px_TX; ) {
            if (!TILE(tiles, tx, ty)) { tx++; continue; }
            int x0 = tx;
            while (tx < px_TX && TILE(tiles, tx, ty)) tx++;
            int x1 = tx, ty1;
            for (ty1 = ty + 1; ty1 < px_TY; ty1++) {
                bool same = (x0 == 0 || !TILE(tiles, x0 - 1, ty1)) && (x1 == px_TX || !TILE(tiles, x1, ty1));
                for (int k = x0; k < x1 && same; k++) same = TILE(tiles, k, ty1);
                if (!same) break;
                for (int k = x0; k < x1; k++) TILE(tiles, k, ty1) = false;
            }
            for (int yy = ty; yy < ty1; yy++)
                for (int k = x0; k < x1; k++) TILE(px_tileD, k, yy) = distance;
            draw_rect(o, x0 * TW, ty * TH, (x1 - x0) * TW, (ty1 - ty) * TH, distance);
        }
    return true;
}

static bool draw_changes(outbuf_t *o, float distance, bool *scratch)
{
    for (int ty = 0; ty < px_TY; ty++)
        for (int tx = 0; tx < px_TX; tx++) {
            bool ch = !px_haveRef || tile_changed(tx, ty);
            TILE(scratch, tx, ty) = ch;
            TILE(px_tileStill, tx, ty) = ch ? 0 : TILE(px_tileStill, tx, ty) + 1;
        }
    px_haveRef = true;
    return draw_tiles(o, scratch, distance);
}

/* Settled tiles once more, sharp; and, now and then, ones left a little off by changes too small to send (a
 * fade's last steps, a dot's last fraction of a pixel), so nothing stays slightly wrong. */
static bool draw_sharper(outbuf_t *o, bool *scratch)
{
    for (int ty = 0; ty < px_TY; ty++)
        for (int tx = 0; tx < px_TX; tx++) {
            int still = TILE(px_tileStill, tx, ty);
            TILE(scratch, tx, ty) = still >= SETTLE_FRAMES &&
                (TILE(px_tileD, tx, ty) > SHARP_DISTANCE + 0.01f || (still >= TIDY_FRAMES && tile_differs(tx, ty)));
        }
    return draw_tiles(o, scratch, SHARP_DISTANCE);
}

/* ------------------------------------------------- if the door falls over --- */

static const char CRASH_RESET[] = "\033[?1003l\033[?1006l\033[?1000l\033[0m\033[2J\033[H\033[?25h";
static const int CRASH_SIGNALS[] = { SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT };

static void crash_reset(int sig)
{
    ssize_t n = write(STDOUT_FILENO, CRASH_RESET, sizeof(CRASH_RESET) - 1);
    (void)n;
    signal(sig, SIG_DFL);
    raise(sig);
}

static void crash_guard(bool on)
{
    for (size_t i = 0; i < sizeof CRASH_SIGNALS / sizeof CRASH_SIGNALS[0]; i++)
        signal(CRASH_SIGNALS[i], on ? crash_reset : SIG_DFL);
}

/* -------------------------------------------------------------- the run --- */

bool pix_sats_run(const pix_caps_t *caps, SatUserPrefs *prefs)
{
    bool alive = true;
    px_prefs = prefs;
    px_blob = caps->blob;
    px_W = caps->px_w > MAX_PX_W ? MAX_PX_W : caps->px_w;
    px_H = caps->px_h > MAX_PX_H ? MAX_PX_H : caps->px_h;
    if (px_W < 320 || px_H < 200) { px_W = 640; px_H = 400; }
    px_cellW = px_W / 80 > 0 ? px_W / 80 : 8;
    px_cellH = px_H / 25 > 0 ? px_H / 25 : 16;
    px_TX = (px_W + TW - 1) / TW;
    px_TY = (px_H + TH - 1) / TH;
    px_rgb = malloc((size_t)px_W * px_H * 3);
    px_ref = malloc((size_t)px_W * px_H * 3);
    px_tileD = calloc((size_t)px_TX * px_TY, sizeof *px_tileD);
    px_tileStill = calloc((size_t)px_TX * px_TY, sizeof *px_tileStill);
    bool *scratch = calloc((size_t)px_TX * px_TY, sizeof *scratch);
    if (!px_rgb || !px_ref || !px_tileD || !px_tileStill || !scratch) return true;
    if (!px_earth) load_earth_file();

    FILE *log = NULL;
    const char *logPath = getenv("SATDOOR_LOG");
    if (logPath && *logPath) log = fopen(logPath, "a");
    else {
        char path[700];
        sat_saves_dir(path, sizeof path);
        strncat(path, "/jxl.log", sizeof path - strlen(path) - 1);
        log = fopen(path, "w");
    }
    if (log) fprintf(log, "jxl: screen %dx%d (cells %dx%d), pictures %s\n", px_W, px_H, px_cellW, px_cellH,
                     px_blob ? "in the command" : "through the cache");

    pthread_t thread;
    px_stop = false;
    if (pthread_create(&thread, NULL, worker, NULL) != 0) return true;

    outbuf_t out = { 0 };
    /* Screen cleared, cursor hidden, mouse reporting on: buttons, motion (for hover) and SGR coordinates */
    out_str(&out, "\033[0m\033[2J\033[H\033[?25l\033[?1000h\033[?1003h\033[?1006h");
    px_write_all(out.data, out.len);
    crash_guard(true);

    px_start = px_inputAt = px_now();
    px_quit = false;
    trace_init();
    trace_on_resize(RW, RH);
    trace_on_data("earth=local", 11);     /* the hash only names it: trace_asset_* has the one asset */
    trace_on_data("start", 5);

    float distance = START_DISTANCE;
    long now = px_now(), tickAt = 0, nextFrameAt = 0, controlAt = now, statsAt = now;
    long frames = 0, bytes = 0, periodFrames = 0, periodSkipped = 0;
    unsigned sentSeq = 0;
    for (;;) {
        struct timeval tv = { 0, 3000 };
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(STDIN_FILENO, &fds);
        if (select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv) > 0) {
            unsigned char buf[512];
            ssize_t n = read(STDIN_FILENO, buf, sizeof buf);
            if (n == 0 || (n < 0 && errno != EINTR && errno != EAGAIN)) { alive = false; break; }
            if (n > 0) take_input(buf, (int)n, px_now());
        } else {
            take_input(NULL, 0, px_now());
        }
        now = px_now();
        if (px_quit || door_time_remaining() <= 0) break;

        deliver();
        /* Left alone, the map is drawn (and sent) only IDLE_FPS times a second */
        bool resting = now - px_inputAt > IDLE_AFTER_MS;
        if (now - tickAt >= (resting ? 1000 / IDLE_FPS : px_tickMs)) {
            tickAt = now;
            trace_update();
            if (px_quit) break;
        }

        if (px_fly.count > 0 && now - px_fly.sent[px_fly.head] > STALL_MS) fly_reset();

        out.len = 0;
        if (px_frame && now >= nextFrameAt) {
            bool fresh = px_frameSeq != sentSeq;
            bool idle = px_fly.count == 0 && periodSkipped == 0;
            if ((fresh || idle) && window_open()) {
                bool sent = false;
                if (fresh) {
                    to_screen();
                    sentSeq = px_frameSeq;
                    sent = draw_changes(&out, distance, scratch);
                }
                if (idle) sent |= draw_sharper(&out, scratch);
                if (sent) {
                    out_str(&out, "\033[6n");
                    px_frameAvg = px_frameAvg * 0.9 + (double)out.len * 0.1;
                    fly_push(now, out.len);
                    frames++;
                    periodFrames++;
                    nextFrameAt = now + 1000 / (px_blob ? TARGET_FPS : CACHE_FPS);
                }
            } else if (fresh) {
                periodSkipped++;
            }
        }
        if (out.len > 0) {
            if (!px_write_all(out.data, out.len)) { alive = false; break; }
            bytes += (long)out.len;
        }

        if (now - controlAt >= CONTROL_MS) {
            long rtt = px_rttN > 0 ? px_rttSum / px_rttN : 0;
            long queue = px_rttN > 0 ? rtt - rtt_min() : 0;
            double sample = (double)px_acked / (double)(now - controlAt);
            px_rate = sample > px_rate ? sample : px_rate * 0.95 + sample * 0.05;
            px_acked = 0;
            bool behind = periodSkipped > periodFrames / 4 + 1 || queue > 80;
            bool keepingUp = periodSkipped <= 1 && queue < 40;
            if (behind) distance *= 1.2f;
            else if (keepingUp) distance *= 0.93f;
            if (distance < MIN_DISTANCE) distance = MIN_DISTANCE;
            if (distance > MAX_DISTANCE) distance = MAX_DISTANCE;
            px_rttSum = px_rttN = 0;
            periodFrames = periodSkipped = 0;
            controlAt = now;
        }
        if (log && now - statsAt >= 5000) {
            long ms = now - statsAt;
            fprintf(log, "jxl: %ld fps, %ld KB/s, distance %.2f, rtt floor %ld ms, link %.0f KB/s\n",
                    frames * 1000 / ms, bytes * 1000 / ms / 1024, distance, rtt_min(), px_rate * 1000 / 1024);
            fflush(log);
            frames = bytes = 0;
            statsAt = now;
        }
    }

    pthread_mutex_lock(&px_lock);
    px_stop = true;
    pthread_cond_signal(&px_wake);
    pthread_mutex_unlock(&px_lock);
    /* A fetch in progress can take a while (curl's own timeout): don't keep the caller waiting for it */
    pthread_detach(thread);

    if (alive) {
        out.len = 0;
        out_str(&out, "\033[?1003l\033[?1006l\033[?1000l\033[0m\033[2J\033[H\033[?25h");
        px_write_all(out.data, out.len);
    }
    crash_guard(false);
    free(out.data);
    if (log) fclose(log);
    return alive;
}
