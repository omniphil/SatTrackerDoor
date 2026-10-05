/*
 * main.c -- TERMinator Satellite Tracker, as a BBS door.
 *
 * Where the ISS and friends are right now, and when they pass over your town:
 *   TRACE  the door sends a module, the Earth pictures (once, cached) and the
 *          orbits; TERMinator draws a live map of the day and night Earth with
 *          the satellites moving across it, their tracks, and your passes.
 *   ANSI   the same live map in 24-bit half blocks, and the same passes.
 *
 * The same start menu as the Weather and News doors: detect quietly, then offer
 * TRACE or ANSI, remembering the caller's choice.
 */
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "ansi_sats.h"
#include "door.h"
#include "sats.h"
#include "trace_door.h"

#define CSI "\033["
#define MODULE_ID "sats"

enum { CHOICE_TRACE = 1, CHOICE_ANSI = 2 };
enum { TERM_PLAIN = 0, TERM_TRACE_OLD, TERM_TRACE };

static SatUserPrefs g_prefs;

/* ------------------------------------------------------------- screens --- */

static void cls(void)
{
    door_write(CSI "0m" CSI "2J" CSI "H");
}

static void title(void)
{
    cls();
    door_write(CSI "1;36m"
               "        ===============================================\r\n"
               "        " CSI "1;35m" "T E R M" CSI "1;36m" " i n a t o r   " CSI "1;37m" "S A T E L L I T E S\r\n"
               CSI "1;36m"
               "        ===============================================\r\n" CSI "0m");
    door_write(CSI "1;34m" "                    BBS door by JSONBourne\r\n" CSI "0m" "\r\n");
}

static void write_terminator(void)
{
    door_write(CSI "1;35m" "TERM" CSI "1;36m" "inator" CSI "0m");
}

static void press_any_key(void)
{
    door_write(CSI "0;37m\r\n  Press any key to return to the BBS...\r\n" CSI "0m");
    door_read_char();
}

/* ------------------------------------------------------- the start menu --- */

static tdoor_blob_t g_module, g_earth;
static bool g_haveModule = false;

static void on_module_message(const unsigned char *data, size_t len);

/* Asks the terminal what it is: TERMinator answers in milliseconds, others
 * never, and the question gives up after half a second. TRACE is offered only
 * with mouse=1 (1.1.3+), since the module uses the mouse. */
static int detect_terminal(void)
{
    tdoor_init(MODULE_ID, on_module_message);
    g_haveModule = tdoor_load_blob("sats.wasm", &g_module, 8 * 1024 * 1024) &&
                   tdoor_load_blob("earth.bin", &g_earth, 8 * 1024 * 1024);
    if (!g_haveModule) return TERM_PLAIN;
    static const char *const need[] = { "send=1", "assets=1", NULL };
    if (!tdoor_detect(need)) return TERM_PLAIN;
    return tdoor_has("mouse=1") ? TERM_TRACE : TERM_TRACE_OLD;
}

static void draw_menu(int term, int recommended)
{
    char line[32];
    title();
    door_write(CSI "0;37m  Choose how to watch the sky:\r\n\r\n");
    if (term == TERM_TRACE)
        door_write(CSI "1;37m  [1] " CSI "1;32mTRACE" CSI "1;37m graphics   " CSI "0;37m(live globe, mouse)    "
                   CSI "1;32mDETECTED\r\n");
    else if (term == TERM_TRACE_OLD)
        door_write(CSI "1;30m  [1] TRACE graphics   (live globe, mouse)    " CSI "1;33mUPDATE " CSI "1;35mTERM"
                   CSI "1;36minator" CSI "1;33m to 1.1.3+\r\n");
    else
        door_write(CSI "1;30m  [1] TRACE graphics   (live globe, mouse)    NOT FOUND (needs " CSI "1;35mTERM"
                   CSI "1;36minator" CSI "1;30m)\r\n");
    door_write(CSI "1;37m  [2] ANSI 24-bit      " CSI "0;37m(any modern terminal)  " CSI "1;32mSUPPORTED\r\n\r\n");
    snprintf(line, sizeof(line), "[%d]", recommended);
    door_write(CSI "0;37m  Enter = " CSI "1;37m");
    door_write(line);
    door_write(CSI "0;37m     Q = back to the BBS " CSI "0m");
}

static int choose_display(int term)
{
    bool trace = term == TERM_TRACE;
    int recommended = trace ? CHOICE_TRACE : CHOICE_ANSI;
    if (g_prefs.display == CHOICE_ANSI || (g_prefs.display == CHOICE_TRACE && trace))
        recommended = g_prefs.display;
    for (;;) {
        draw_menu(term, recommended);
        int c = door_read_char();
        if (c < 0 || c == 'q' || c == 'Q' || c == 27) return 0;
        if (c == '\r' || c == '\n') c = '0' + recommended;
        if (c == '1' && !trace) continue;
        if (c == '1' || c == '2') {
            g_prefs.display = c - '0';
            sat_save_prefs(&g_prefs);
            return c - '0';
        }
    }
}

/* ------------------------------------------------------------ TRACE link --- */

static struct {
    bool ready, search, pick, group, select;
    char query[SAT_SEARCH_LEN + 1];
    int pickIndex, groupIndex;
    uint32_t norad;
} g_req;

static SatPlace g_places[SAT_MAX_PLACES];
static int g_placeCount = 0;

static void on_module_message(const unsigned char *data, size_t len)
{
    if (len < sizeof(SatMsgHeader)) return;
    SatMsgHeader h;
    memcpy(&h, data, sizeof h);
    const unsigned char *body = data + sizeof h;
    if (len - sizeof h < h.bytes) return;
    switch (h.type) {
    case SAT_OUT_READY: g_req.ready = true; break;
    case SAT_OUT_GROUP:
        if (h.bytes >= 1) { g_req.groupIndex = body[0]; g_req.group = true; }
        break;
    case SAT_OUT_SEARCH:
        if (h.bytes >= 1) {
            size_t n = body[0];
            if (n > h.bytes - 1) n = h.bytes - 1;
            if (n > SAT_SEARCH_LEN) n = SAT_SEARCH_LEN;
            memcpy(g_req.query, body + 1, n);
            g_req.query[n] = 0;
            g_req.search = true;
        }
        break;
    case SAT_OUT_PICK:
        if (h.bytes >= 1) { g_req.pickIndex = body[0]; g_req.pick = true; }
        break;
    case SAT_OUT_SELECT:
        if (h.bytes >= 4) { memcpy(&g_req.norad, body, 4); g_req.select = true; }
        break;
    default:
        break;
    }
}

static void send_packet(uint8_t type, uint8_t flags, uint16_t count, const void *payload, size_t len)
{
    unsigned char buf[sizeof(SatMsgHeader) + SAT_MAX_PAYLOAD];
    if (len > SAT_MAX_PAYLOAD) return;
    SatMsgHeader h = { type, flags, count, (uint32_t)len };
    memcpy(buf, &h, sizeof h);
    if (payload && len) memcpy(buf + sizeof h, payload, len);
    tdoor_send("msg", buf, sizeof h + len);
}

static void send_status(int kind, const char *text)
{
    unsigned char buf[2 + 120];
    size_t n = strlen(text);
    if (n > 120) n = 120;
    buf[0] = (unsigned char)kind;
    buf[1] = (unsigned char)n;
    memcpy(buf + 2, text, n);
    send_packet(SAT_IN_STATUS, 0, 1, buf, 2 + n);
}

static void send_prefs(void)
{
    SatPrefs p;
    memset(&p, 0, sizeof p);
    p.doorNow = (uint32_t)time(NULL);
    p.utcOffset = g_prefs.utcOffset > -900000 ? g_prefs.utcOffset : 0;
    p.lat1e4 = (int32_t)(g_prefs.lat * 1e4);
    p.lon1e4 = (int32_t)(g_prefs.lon * 1e4);
    p.selected = g_prefs.selected;
    p.group = (uint8_t)g_prefs.group;
    p.firstRun = g_prefs.havePlace ? 0 : 1;
    memcpy(p.place, g_prefs.place, SAT_NAME_LEN);
    send_packet(SAT_IN_PREFS, 0, 1, &p, sizeof p);
}

static void send_group(int group)
{
    static OrbElements e[SAT_MAX];
    char err[200];
    send_status(SAT_STATUS_BUSY, "Fetching orbits...");
    int n = sat_group(group, e, SAT_MAX, err, sizeof err);
    if (n < 0) { send_status(SAT_STATUS_ERROR, err); return; }
    SatWire w[42];
    int k = 0, first = 1;
    for (int i = 0; i <= n; i++) {
        if (i == n || k == 42) {
            send_packet(SAT_IN_ELEMS, first ? 1 : 0, (uint16_t)k, w, (size_t)k * sizeof(SatWire));
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
    send_packet(SAT_IN_DONE, 0, 1, &g, 1);
    char msg[80];
    snprintf(msg, sizeof msg, "%d satellites in %s", n, SAT_GROUP_NAME[group]);
    send_status(SAT_STATUS_INFO, msg);
}

static void handle_requests(void)
{
    if (g_req.ready) {
        g_req.ready = false;
        if (g_prefs.havePlace || g_prefs.utcOffset < -900000) {
            int off;
            if (sat_utc_offset(g_prefs.lat, g_prefs.lon, &off)) { g_prefs.utcOffset = off; sat_save_prefs(&g_prefs); }
        }
        send_prefs();
        send_group(g_prefs.group);
    }
    if (g_req.group) {
        g_req.group = false;
        if (g_req.groupIndex >= 0 && g_req.groupIndex < GRP_COUNT) {
            g_prefs.group = g_req.groupIndex;
            sat_save_prefs(&g_prefs);
            send_group(g_prefs.group);
        }
    }
    if (g_req.search) {
        g_req.search = false;
        int n = sat_search(g_req.query, g_places, SAT_MAX_PLACES);
        if (n < 0) { g_placeCount = 0; send_status(SAT_STATUS_ERROR, "Couldn't reach the place search. Try again."); }
        else { g_placeCount = n; send_packet(SAT_IN_PLACES, 0, (uint16_t)n, g_places, (size_t)n * sizeof(SatPlace)); }
    }
    if (g_req.pick) {
        g_req.pick = false;
        if (g_req.pickIndex >= 0 && g_req.pickIndex < g_placeCount) {
            const SatPlace *p = &g_places[g_req.pickIndex];
            memcpy(g_prefs.place, p->name, SAT_NAME_LEN);
            g_prefs.lat = p->lat1e4 / 1e4;
            g_prefs.lon = p->lon1e4 / 1e4;
            g_prefs.havePlace = true;
            int off;
            if (sat_utc_offset(g_prefs.lat, g_prefs.lon, &off)) g_prefs.utcOffset = off;
            sat_save_prefs(&g_prefs);
            send_prefs();
        }
    }
    if (g_req.select) {
        g_req.select = false;
        g_prefs.selected = g_req.norad;
        sat_save_prefs(&g_prefs);
    }
}

static void upload_progress(int percent)
{
    char buf[96];
    snprintf(buf, sizeof buf, "\r" CSI "0;36m  Sending the Earth pictures (first visit only)... %3d%%" CSI "0m", percent);
    door_write(buf);
}

static int play_trace(void)
{
    title();
    door_write(CSI "1;32m  ");
    write_terminator();
    door_write(CSI "1;32m found. Opening the satellite tracker...\r\n\r\n" CSI "0m");
    if (!tdoor_send_asset(&g_earth, upload_progress) || !tdoor_open(&g_module, "exclusive=1", NULL)) {
        door_write(CSI "1;33m\r\n  The tracker couldn't be started on your terminal.\r\n" CSI "0m");
        press_any_key();
        return 1;
    }
    cls();
    char buf[96];
    snprintf(buf, sizeof buf, "earth=%s", g_earth.hash);
    tdoor_send_text(buf);
    tdoor_send_text("start");

    memset(&g_req, 0, sizeof g_req);
    for (;;) {
        int reply = tdoor_wait_reply(200);
        if (reply == TDOOR_REPLY_CLOSED || !tdoor_is_open()) break;
        handle_requests();
        if (door_time_remaining() <= 0) break;
    }
    tdoor_close("quit", 2000);

    /* A module that failed says so (a non-zero code, usually a reason); a quick
     * quit is just a quick quit. */
    if (tdoor_last_close_code() != 0 || tdoor_last_close_reason()[0]) {
        title();
        door_write(CSI "1;31m  The picture stopped with an error.\r\n" CSI "0m");
        const char *why = tdoor_last_close_reason();
        char line[200];
        snprintf(line, sizeof line, CSI "1;33m  Reason: %s\r\n" CSI "0m", (why && *why) ? why : "(none given)");
        door_write(line);
        snprintf(line, sizeof line, CSI "1;30m  Your TERMinator offers: %.100s\r\n" CSI "0m", tdoor_info());
        door_write(line);
        door_write(CSI "0;37m  Please send those two lines to the sysop, or choose ANSI next time.\r\n" CSI "0m");
        press_any_key();
        return 1;
    }
    cls();
    return 0;
}

int main(int argc, char *argv[])
{
    door_init(argc > 1 ? argv[1] : NULL);
    sat_load_config();
    sat_load_prefs(&g_prefs);

    title();
    int term = detect_terminal();
    int choice = choose_display(term);
    int status = 0;
    if (choice == CHOICE_TRACE) status = play_trace();
    else if (choice == CHOICE_ANSI) ansi_sats_run(&g_prefs);

    door_cleanup();
    return status;
}
