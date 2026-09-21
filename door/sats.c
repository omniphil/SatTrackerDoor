/*
 * sats.c - the Satellite Tracker door's data. See sats.h.
 *
 * HTTP goes through the curl command, run with fork and exec so the caller's
 * search text never passes through a shell. The place search is the Weather
 * door's; orbit elements come from CelesTrak, which asks that the same data is
 * not fetched more than every two hours, so each group is cached for
 * cache_hours (at least 2).
 */
#include "sats.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "door.h"
#include "json.h"

#define USER_AGENT   "TERMinator-Satellite-Door/1.0 (+https://github.com/omniphil)"
#define MAX_REPLY    (1024 * 1024)
#define HTTP_TIMEOUT "15"

SatConfig sat_config;

/* ------------------------------------------------------------- paths --- */

const char *sat_beside_exe(const char *name, char *out, size_t size)
{
    char path[1024];
    ssize_t len = readlink("/proc/self/exe", path, sizeof path - 1);
    if (len > 0) {
        path[len] = '\0';
        char *slash = strrchr(path, '/');
        if (slash) {
            slash[1] = '\0';
            snprintf(out, size, "%s%s", path, name);
            return out;
        }
    }
    snprintf(out, size, "%s", name);
    return out;
}

static void trim(char *s)
{
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
    char *b = s;
    while (isspace((unsigned char)*b)) b++;
    if (b != s) memmove(s, b, strlen(b) + 1);
}

static void read_kv(const char *path, void (*fn)(const char *k, const char *v, void *ctx), void *ctx)
{
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[700];
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#') continue;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        trim(line);
        trim(eq + 1);
        if (line[0]) fn(line, eq + 1, ctx);
    }
    fclose(f);
}

static void copy_name(char *dst, const char *src)
{
    size_t n = strlen(src);
    if (n > SAT_NAME_LEN - 1) n = SAT_NAME_LEN - 1;
    memcpy(dst, src, n);
    memset(dst + n, 0, SAT_NAME_LEN - n);
}

static void write_file(const char *path, const void *data, size_t len)
{
    char tmp[1300];
    snprintf(tmp, sizeof tmp, "%.1200s.%d.tmp", path, (int)getpid());
    FILE *fp = fopen(tmp, "wb");
    if (!fp) return;
    fwrite(data, 1, len, fp);
    fclose(fp);
    rename(tmp, path);
}

static char *read_file(const char *path)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    fseek(fp, 0, SEEK_END);
    long n = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (n <= 0 || n > MAX_REPLY) { fclose(fp); return NULL; }
    char *buf = malloc((size_t)n + 1);
    if (buf && fread(buf, 1, (size_t)n, fp) != (size_t)n) { free(buf); buf = NULL; }
    fclose(fp);
    if (buf) buf[n] = 0;
    return buf;
}

/* ------------------------------------------------------------ config --- */

static void config_kv(const char *k, const char *v, void *ctx)
{
    (void)ctx;
    if (!strcmp(k, "default_place"))      copy_name(sat_config.place, v);
    else if (!strcmp(k, "default_lat"))   sat_config.lat = atof(v);
    else if (!strcmp(k, "default_lon"))   sat_config.lon = atof(v);
    else if (!strcmp(k, "cache_hours"))   sat_config.cacheHours = atoi(v);
    else if (!strcmp(k, "weather_saves")) snprintf(sat_config.weatherSaves, sizeof sat_config.weatherSaves, "%s", v);
}

void sat_load_config(void)
{
    memset(&sat_config, 0, sizeof sat_config);
    copy_name(sat_config.place, "New York, New York");
    sat_config.lat = 40.7128;
    sat_config.lon = -74.0060;
    sat_config.cacheHours = 8;
    char path[1100];
    read_kv(sat_beside_exe("sats.ini", path, sizeof path), config_kv, NULL);
    if (sat_config.cacheHours < 2) sat_config.cacheHours = 2;
}

/* ------------------------------------------------------------- prefs --- */

/* saves/<handle>-<usernum>, the same shape as the other doors. */
static void user_dir(const char *root, char *out, size_t size, bool make)
{
    char who[MAX_USERNAME];
    snprintf(who, sizeof who, "%s", door_info.handle[0] ? door_info.handle : "Player");
    for (char *c = who; *c; c++)
        if (*c == '/' || *c == '\\' || *c == ' ' || *c == '.') *c = '_';
    if (make) mkdir(root, 0755);
    snprintf(out, size, "%s/%s-%d", root, who, door_info.user_record);
    if (make) mkdir(out, 0755);
}

static const char *prefs_path(char *out, size_t size)
{
    char root[1100], dir[1300];
    sat_beside_exe("saves", root, sizeof root);
    user_dir(root, dir, sizeof dir, true);
    snprintf(out, size, "%s/prefs", dir);
    return out;
}

static void prefs_kv(const char *k, const char *v, void *ctx)
{
    SatUserPrefs *p = ctx;
    if (!strcmp(k, "place"))           { copy_name(p->place, v); p->havePlace = true; }
    else if (!strcmp(k, "lat"))        p->lat = atof(v);
    else if (!strcmp(k, "lon"))        p->lon = atof(v);
    else if (!strcmp(k, "utc_offset")) p->utcOffset = atoi(v);
    else if (!strcmp(k, "group"))      { p->group = atoi(v); if (p->group < 0 || p->group >= GRP_COUNT) p->group = 0; }
    else if (!strcmp(k, "selected"))   p->selected = (uint32_t)strtoul(v, NULL, 10);
    else if (!strcmp(k, "display"))    p->display = atoi(v);
}

/* The Weather door's prefs for this caller: only place, lat and lon are taken. */
static void weather_kv(const char *k, const char *v, void *ctx)
{
    SatUserPrefs *p = ctx;
    if (!strcmp(k, "place"))    { copy_name(p->place, v); p->havePlace = true; }
    else if (!strcmp(k, "lat")) p->lat = atof(v);
    else if (!strcmp(k, "lon")) p->lon = atof(v);
}

void sat_load_prefs(SatUserPrefs *p)
{
    memset(p, 0, sizeof *p);
    copy_name(p->place, sat_config.place);
    p->lat = sat_config.lat;
    p->lon = sat_config.lon;
    p->utcOffset = -1000000;           /* unknown: looked up when the door starts */
    p->group = GRP_STATIONS;
    p->selected = SAT_ISS;
    char path[1400];
    read_kv(prefs_path(path, sizeof path), prefs_kv, p);
    if (!p->havePlace && sat_config.weatherSaves[0]) {
        /* A caller who has told the Weather door where they live starts there. */
        char dir[1300], wp[1400];
        user_dir(sat_config.weatherSaves, dir, sizeof dir, false);
        snprintf(wp, sizeof wp, "%s/prefs", dir);
        read_kv(wp, weather_kv, p);
    }
}

void sat_save_prefs(const SatUserPrefs *p)
{
    char path[1400], buf[400];
    prefs_path(path, sizeof path);
    int n = 0;
    if (p->havePlace)
        n += snprintf(buf + n, sizeof buf - (size_t)n, "place = %s\nlat = %.4f\nlon = %.4f\n", p->place, p->lat, p->lon);
    n += snprintf(buf + n, sizeof buf - (size_t)n, "utc_offset = %d\ngroup = %d\nselected = %u\ndisplay = %d\n",
                  p->utcOffset, p->group, p->selected, p->display);
    write_file(path, buf, (size_t)n);
}

/* -------------------------------------------------------------- http --- */

/* GETs url into a malloc'd, NUL-terminated buffer. NULL on any failure. */
static char *http_get(const char *url)
{
    int pipefd[2];
    if (pipe(pipefd) < 0) return NULL;
    pid_t pid = fork();
    if (pid < 0) { close(pipefd[0]); close(pipefd[1]); return NULL; }
    if (pid == 0) {
        dup2(pipefd[1], STDOUT_FILENO);
        int devnull = open("/dev/null", 1);
        if (devnull >= 0) dup2(devnull, STDERR_FILENO);
        /* stdin is the caller's line: curl must never read it. */
        int devin = open("/dev/null", 0);
        if (devin >= 0) dup2(devin, STDIN_FILENO);
        close(pipefd[0]);
        close(pipefd[1]);
        execlp("curl", "curl", "-sSfL", "--compressed", "--max-time", HTTP_TIMEOUT,
               "-A", USER_AGENT, url, (char *)NULL);
        _exit(127);
    }
    close(pipefd[1]);
    size_t cap = 16384, len = 0;
    char *buf = malloc(cap);
    for (;;) {
        if (!buf) break;
        if (len + 4096 + 1 > cap) {
            if (cap >= MAX_REPLY) break;
            char *nb = realloc(buf, cap * 2);
            if (!nb) { free(buf); buf = NULL; break; }
            buf = nb;
            cap *= 2;
        }
        ssize_t n = read(pipefd[0], buf + len, cap - len - 1);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        len += (size_t)n;
    }
    close(pipefd[0]);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    if (!buf) return NULL;
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0 || len == 0) { free(buf); return NULL; }
    buf[len] = 0;
    return buf;
}

static void url_encode(const char *in, char *out, size_t size)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;
    for (; *in && o + 4 < size; in++) {
        unsigned char c = (unsigned char)*in;
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out[o++] = (char)c;
        else { out[o++] = '%'; out[o++] = hex[c >> 4]; out[o++] = hex[c & 15]; }
    }
    out[o] = 0;
}

/* ------------------------------------------------------------ search --- */

static const struct { const char *abbr, *name; } US_STATES[] = {
    {"AL","Alabama"},{"AK","Alaska"},{"AZ","Arizona"},{"AR","Arkansas"},{"CA","California"},
    {"CO","Colorado"},{"CT","Connecticut"},{"DE","Delaware"},{"FL","Florida"},{"GA","Georgia"},
    {"HI","Hawaii"},{"ID","Idaho"},{"IL","Illinois"},{"IN","Indiana"},{"IA","Iowa"},
    {"KS","Kansas"},{"KY","Kentucky"},{"LA","Louisiana"},{"ME","Maine"},{"MD","Maryland"},
    {"MA","Massachusetts"},{"MI","Michigan"},{"MN","Minnesota"},{"MS","Mississippi"},
    {"MO","Missouri"},{"MT","Montana"},{"NE","Nebraska"},{"NV","Nevada"},{"NH","New Hampshire"},
    {"NJ","New Jersey"},{"NM","New Mexico"},{"NY","New York"},{"NC","North Carolina"},
    {"ND","North Dakota"},{"OH","Ohio"},{"OK","Oklahoma"},{"OR","Oregon"},{"PA","Pennsylvania"},
    {"RI","Rhode Island"},{"SC","South Carolina"},{"SD","South Dakota"},{"TN","Tennessee"},
    {"TX","Texas"},{"UT","Utah"},{"VT","Vermont"},{"VA","Virginia"},{"WA","Washington"},
    {"WV","West Virginia"},{"WI","Wisconsin"},{"WY","Wyoming"},{"DC","District of Columbia"},
};

static const char *state_abbr(const char *name)
{
    for (size_t i = 0; i < sizeof US_STATES / sizeof US_STATES[0]; i++)
        if (!strcasecmp(US_STATES[i].name, name)) return US_STATES[i].abbr;
    return NULL;
}

/* Does the part after the comma ("OR", "Oregon", "UK", "France") fit this result? */
static bool qualifier_matches(const json_node *r, const char *q)
{
    if (!q[0]) return true;
    const char *admin1  = json_str(json_get(r, "admin1"), "");
    const char *country = json_str(json_get(r, "country"), "");
    const char *cc      = json_str(json_get(r, "country_code"), "");
    const char *ab      = state_abbr(admin1);
    if (!strcasecmp(q, admin1) || !strcasecmp(q, country) || !strcasecmp(q, cc)) return true;
    if (ab && !strcasecmp(q, ab)) return true;
    if (!strcasecmp(q, "UK") && !strcasecmp(cc, "GB")) return true;
    if ((!strcasecmp(q, "USA") || !strcasecmp(q, "America")) && !strcasecmp(cc, "US")) return true;
    return false;
}

/* "Portland, Oregon" in the US, "Hamburg, DE" abroad; never "Hamburg, Hamburg". */
static void place_name(const json_node *r, char *out)
{
    const char *name   = json_str(json_get(r, "name"), "?");
    const char *admin1 = json_str(json_get(r, "admin1"), "");
    const char *cc     = json_str(json_get(r, "country_code"), "");
    const char *country = json_str(json_get(r, "country"), "");
    bool us = !strcmp(cc, "US");
    char buf[160];
    if (us)
        snprintf(buf, sizeof buf, "%s%s%s", name, admin1[0] ? ", " : "", admin1);
    /* Skip a region that repeats the town ("Free and Hanseatic City of Hamburg")
     * or would push the name past what the screen shows. */
    else if (admin1[0] && !strstr(admin1, name) && strlen(name) + strlen(admin1) < 30)
        snprintf(buf, sizeof buf, "%s, %s, %s", name, admin1, cc[0] ? cc : country);
    else
        snprintf(buf, sizeof buf, "%s, %s", name, country[0] ? country : cc);
    copy_name(out, buf);
}

int sat_search(const char *query, SatPlace *out, int max)
{
    char name[SAT_SEARCH_LEN + 1], qual[SAT_SEARCH_LEN + 1] = "";
    snprintf(name, sizeof name, "%s", query);
    char *comma = strchr(name, ',');
    if (comma) {
        *comma = 0;
        snprintf(qual, sizeof qual, "%s", comma + 1);
        trim(qual);
    }
    trim(name);
    if (!name[0]) return 0;

    char enc[160], url[400];
    url_encode(name, enc, sizeof enc);
    /* Ask for plenty, so a qualifier can pick Springfield, IL out of the pile. */
    snprintf(url, sizeof url,
             "https://geocoding-api.open-meteo.com/v1/search?name=%s&count=%d&language=en&format=json",
             enc, qual[0] ? 30 : max);
    char *body = http_get(url);
    if (!body) return -1;
    json_node *root = json_parse(body);
    free(body);
    if (!root) return -1;

    const json_node *results = json_get(root, "results");
    int n = 0;
    /* Two passes: the ones that fit the qualifier, then (if there's room and
     * nothing fitted) the rest, so a typo in the state still finds the town. */
    for (int pass = 0; pass < 2 && n < max; pass++) {
        if (pass == 1 && (n > 0 || !qual[0])) break;
        for (const json_node *r = results ? results->child : NULL; r && n < max; r = r->next) {
            if (pass == 0 && !qualifier_matches(r, qual)) continue;
            out[n].lat1e4 = (int32_t)lround(json_num(json_get(r, "latitude"), 0) * 1e4);
            out[n].lon1e4 = (int32_t)lround(json_num(json_get(r, "longitude"), 0) * 1e4);
            place_name(r, out[n].name);
            n++;
        }
    }
    json_free(root);
    return n;
}

/* ----------------------------------------------------------- timezone --- */

bool sat_utc_offset(double lat, double lon, int *offset)
{
    char url[300];
    snprintf(url, sizeof url, "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
             "&current=is_day&timezone=auto&forecast_days=1", lat, lon);
    char *body = http_get(url);
    if (!body) return false;
    json_node *root = json_parse(body);
    free(body);
    const json_node *o = root ? json_get(root, "utc_offset_seconds") : NULL;
    bool ok = o && !json_is_null(o);
    if (ok) *offset = (int)json_num(o, 0);
    json_free(root);
    return ok;
}

/* ------------------------------------------------------------ orbits --- */

int sat_group(int group, OrbElements *out, int max, char *err, size_t errSize)
{
    if (group < 0 || group >= GRP_COUNT) group = 0;
    char dir[1100], path[1300];
    sat_beside_exe("cache", dir, sizeof dir);
    mkdir(dir, 0755);
    snprintf(path, sizeof path, "%s/%s.tle", dir, SAT_GROUP_KEY[group]);

    struct stat st;
    char *body = NULL;
    if (stat(path, &st) == 0 && time(NULL) - st.st_mtime < sat_config.cacheHours * 3600L)
        body = read_file(path);
    if (!body) {
        char url[200];
        snprintf(url, sizeof url, "https://celestrak.org/NORAD/elements/gp.php?GROUP=%s&FORMAT=tle", SAT_GROUP_KEY[group]);
        body = http_get(url);
        if (body && strstr(body, "\n1 ")) {
            write_file(path, body, strlen(body));
        } else {
            free(body);
            body = read_file(path);        /* CelesTrak unreachable: older elements beat none */
        }
    }
    if (!body) {
        snprintf(err, errSize, "Couldn't reach CelesTrak for the orbits. Try again in a minute.");
        return -1;
    }
    /* Name line, line 1, line 2, repeated. */
    int n = 0;
    char *lines[3];
    int k = 0;
    for (char *line = strtok(body, "\n"); line && n < max; line = strtok(NULL, "\n")) {
        size_t l = strlen(line);
        if (l && line[l - 1] == '\r') line[l - 1] = 0;
        lines[k++] = line;
        if (k == 3) {
            if (orb_parse_tle(lines[0], lines[1], lines[2], &out[n])) n++;
            k = 0;
        }
    }
    free(body);
    if (!n) snprintf(err, errSize, "CelesTrak sent no satellites for this group.");
    return n ? n : -1;
}
