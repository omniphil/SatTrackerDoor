// Orbits for the Satellite Tracker: shared verbatim by the door (passes, the
// ANSI map) and the TRACE module (positions every frame), so both always agree.
// Pure C, no host calls.
//
//   - SGP4 (Spacetrack Report #3, as refined by Vallado et al. 2006), the model
//     two-line element sets are made for, for near-Earth orbits (period under
//     225 minutes: the ISS, Hubble, weather and ham satellites).
//   - Deep-space orbits (GPS, geostationary) use Kepler plus the Earth's J2
//     drift instead of the full SDP4 model. That is a few tens of km off, which
//     can't be seen on a world map, and nobody plans a sighting of a GPS satellite.
//   - Earth rotation (GMST), latitude/longitude/altitude (WGS-84), the look
//     angles from an observer, the Sun, and a pass finder.
//
// Checked against the reference sgp4 library (tools/check_orbit.py).
#pragma once
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define ORB_PI      3.14159265358979323846
#define ORB_TWOPI   (2.0 * ORB_PI)
#define ORB_DEG     (ORB_PI / 180.0)

// WGS-72, which SGP4 elements are fitted with.
#define ORB_MU      398600.8
#define ORB_RE      6378.135                 // km
#define ORB_XKE     0.0743669161331734       // 60 / sqrt(RE^3 / MU)
#define ORB_J2      0.001082616
#define ORB_J3      -0.00000253881
#define ORB_J4      -0.00000165597
#define ORB_J3OJ2   (ORB_J3 / ORB_J2)

typedef struct {
    char     name[25];
    uint32_t norad;
    double   epoch;          // unix seconds (UTC) of the elements
    double   incl, raan, ecc, argp, mo;   // radians
    double   nRevDay;        // mean motion, revolutions per day
    double   bstar;
} OrbElements;

typedef struct {
    OrbElements e;
    int    deep, isimp, error;
    double no, a, con41, con42, x1mth2, x7thm1, cosio, sinio, eta;
    double cc1, cc4, cc5, d2, d3, d4, delmo, sinmao, t2cof, t3cof, t4cof, t5cof;
    double mdot, argpdot, nodedot, omgcof, xmcof, nodecf, xlcof, aycof;
} OrbSat;

typedef struct { double x, y, z; } OrbVec;

// ------------------------------------------------------------- TLE ---

static int64_t orb_days_from_civil(int y, int m, int d)
{
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;
    int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static double orb_field(const char *line, int from, int len)
{
    char buf[32];
    if (len > 31) len = 31;
    memcpy(buf, line + from, (size_t)len);
    buf[len] = 0;
    return atof(buf);
}

// The TLE "implied decimal" fields: " 14494-3" = 0.14494e-3.
static double orb_implied(const char *line, int from)
{
    char mant[16], exp[4];
    memcpy(mant, line + from, 6);
    mant[6] = 0;
    char sign = mant[0];
    memcpy(exp, line + from + 6, 2);
    exp[2] = 0;
    double m = atof(mant + 1) * 1e-5;
    if (sign == '-') m = -m;
    return m * pow(10.0, atof(exp));
}

// Parses a name line (may be NULL) and the two element lines. 0 on a bad line.
static int orb_parse_tle(const char *name, const char *l1, const char *l2, OrbElements *e)
{
    if (!l1 || !l2 || strlen(l1) < 68 || strlen(l2) < 68 || l1[0] != '1' || l2[0] != '2') return 0;
    memset(e, 0, sizeof *e);
    if (name) {
        size_t n = strlen(name);
        while (n && (name[n - 1] == ' ' || name[n - 1] == '\r' || name[n - 1] == '\n')) n--;
        if (n > 24) n = 24;
        memcpy(e->name, name, n);
        e->name[n] = 0;
    }
    e->norad = (uint32_t)orb_field(l1, 2, 5);
    int yy = (int)orb_field(l1, 18, 2);
    double doy = orb_field(l1, 20, 12);
    int year = yy < 57 ? 2000 + yy : 1900 + yy;
    e->epoch = (double)orb_days_from_civil(year, 1, 1) * 86400.0 + (doy - 1.0) * 86400.0;
    e->bstar = orb_implied(l1, 53);
    e->incl = orb_field(l2, 8, 8) * ORB_DEG;
    e->raan = orb_field(l2, 17, 8) * ORB_DEG;
    char ecc[16] = "0.";
    memcpy(ecc + 2, l2 + 26, 7);
    ecc[9] = 0;
    e->ecc = atof(ecc);
    e->argp = orb_field(l2, 34, 8) * ORB_DEG;
    e->mo = orb_field(l2, 43, 8) * ORB_DEG;
    e->nRevDay = orb_field(l2, 52, 11);
    return e->nRevDay > 0.05;
}

// ------------------------------------------------------------- SGP4 ---

static void orb_init(OrbSat *s, const OrbElements *e)
{
    memset(s, 0, sizeof *s);
    s->e = *e;
    const double x2o3 = 2.0 / 3.0;
    double no_kozai = e->nRevDay * ORB_TWOPI / 1440.0;      // rad/min
    double ecco = e->ecc, inclo = e->incl;
    double eccsq = ecco * ecco, omeosq = 1.0 - eccsq, rteosq = sqrt(omeosq);
    double cosio = cos(inclo), cosio2 = cosio * cosio;
    double ak = pow(ORB_XKE / no_kozai, x2o3);
    double d1 = 0.75 * ORB_J2 * (3.0 * cosio2 - 1.0) / (rteosq * omeosq);
    double del = d1 / (ak * ak);
    double adel = ak * (1.0 - del * del - del * (1.0 / 3.0 + 134.0 * del * del / 81.0));
    del = d1 / (adel * adel);
    double no = no_kozai / (1.0 + del);
    double ao = pow(ORB_XKE / no, x2o3);
    double sinio = sin(inclo), po = ao * omeosq, con42 = 1.0 - 5.0 * cosio2;
    double con41 = -con42 - cosio2 - cosio2;
    double posq = po * po, rp = ao * (1.0 - ecco);
    s->no = no;
    s->a = ao;
    s->con41 = con41;
    s->con42 = con42;
    s->cosio = cosio;
    s->sinio = sinio;
    s->x1mth2 = 1.0 - cosio2;
    s->x7thm1 = 7.0 * cosio2 - 1.0;

    if (ORB_TWOPI / no >= 225.0) {
        // Deep space: Kepler + J2 secular rates (see the header comment).
        s->deep = 1;
        double p = ao * omeosq, n = no;
        s->nodedot = -1.5 * ORB_J2 * n * cosio / (p * p);
        s->argpdot = 0.75 * ORB_J2 * n * (5.0 * cosio2 - 1.0) / (p * p);
        s->mdot = n * (1.0 + 0.75 * ORB_J2 * rteosq * (3.0 * cosio2 - 1.0) / (p * p));
        return;
    }

    s->isimp = rp < 220.0 / ORB_RE + 1.0;
    double sfour = 78.0 / ORB_RE + 1.0;
    double qzms24 = pow((120.0 - 78.0) / ORB_RE, 4);
    double perige = (rp - 1.0) * ORB_RE;
    if (perige < 156.0) {
        sfour = perige - 78.0;
        if (perige < 98.0) sfour = 20.0;
        qzms24 = pow((120.0 - sfour) / ORB_RE, 4);
        sfour = sfour / ORB_RE + 1.0;
    }
    double pinvsq = 1.0 / posq;
    double tsi = 1.0 / (ao - sfour);
    double eta = ao * ecco * tsi, etasq = eta * eta, eeta = ecco * eta;
    double psisq = fabs(1.0 - etasq);
    double coef = qzms24 * pow(tsi, 4), coef1 = coef / pow(psisq, 3.5);
    double cc2 = coef1 * no * (ao * (1.0 + 1.5 * etasq + eeta * (4.0 + etasq)) +
                 0.375 * ORB_J2 * tsi / psisq * con41 * (8.0 + 3.0 * etasq * (8.0 + etasq)));
    double cc1 = e->bstar * cc2, cc3 = 0.0;
    if (ecco > 1.0e-4) cc3 = -2.0 * coef * tsi * ORB_J3OJ2 * no * sinio / ecco;
    double x1mth2 = s->x1mth2;
    s->cc4 = 2.0 * no * coef1 * ao * omeosq *
             (eta * (2.0 + 0.5 * etasq) + ecco * (0.5 + 2.0 * etasq) -
              ORB_J2 * tsi / (ao * psisq) *
              (-3.0 * con41 * (1.0 - 2.0 * eeta + etasq * (1.5 - 0.5 * eeta)) +
               0.75 * x1mth2 * (2.0 * etasq - eeta * (1.0 + etasq)) * cos(2.0 * e->argp)));
    s->cc5 = 2.0 * coef1 * ao * omeosq * (1.0 + 2.75 * (etasq + eeta) + eeta * etasq);
    double cosio4 = cosio2 * cosio2;
    double temp1 = 1.5 * ORB_J2 * pinvsq * no;
    double temp2 = 0.5 * temp1 * ORB_J2 * pinvsq;
    double temp3 = -0.46875 * ORB_J4 * pinvsq * pinvsq * no;
    s->mdot = no + 0.5 * temp1 * rteosq * con41 + 0.0625 * temp2 * rteosq * (13.0 - 78.0 * cosio2 + 137.0 * cosio4);
    s->argpdot = -0.5 * temp1 * con42 + 0.0625 * temp2 * (7.0 - 114.0 * cosio2 + 395.0 * cosio4) +
                 temp3 * (3.0 - 36.0 * cosio2 + 49.0 * cosio4);
    double xhdot1 = -temp1 * cosio;
    s->nodedot = xhdot1 + (0.5 * temp2 * (4.0 - 19.0 * cosio2) + 2.0 * temp3 * (3.0 - 7.0 * cosio2)) * cosio;
    s->omgcof = e->bstar * cc3 * cos(e->argp);
    s->xmcof = ecco > 1.0e-4 ? -x2o3 * coef * e->bstar / eeta : 0.0;
    s->nodecf = 3.5 * omeosq * xhdot1 * cc1;
    s->t2cof = 1.5 * cc1;
    if (fabs(cosio + 1.0) > 1.5e-12) s->xlcof = -0.25 * ORB_J3OJ2 * sinio * (3.0 + 5.0 * cosio) / (1.0 + cosio);
    else s->xlcof = -0.25 * ORB_J3OJ2 * sinio * (3.0 + 5.0 * cosio) / 1.5e-12;
    s->aycof = -0.5 * ORB_J3OJ2 * sinio;
    s->delmo = pow(1.0 + eta * cos(e->mo), 3);
    s->sinmao = sin(e->mo);
    s->eta = eta;
    s->cc1 = cc1;
    if (!s->isimp) {
        double cc1sq = cc1 * cc1;
        s->d2 = 4.0 * ao * tsi * cc1sq;
        double temp = s->d2 * tsi * cc1 / 3.0;
        s->d3 = (17.0 * ao + sfour) * temp;
        s->d4 = 0.5 * temp * ao * tsi * (221.0 * ao + 31.0 * sfour) * cc1;
        s->t3cof = s->d2 + 2.0 * cc1sq;
        s->t4cof = 0.25 * (3.0 * s->d3 + cc1 * (12.0 * s->d2 + 10.0 * cc1sq));
        s->t5cof = 0.2 * (3.0 * s->d4 + 12.0 * cc1 * s->d3 + 6.0 * s->d2 * s->d2 + 15.0 * cc1sq * (2.0 * s->d2 + cc1sq));
    }
}

// Solves Kepler's equation for the eccentric anomaly.
static double orb_kepler(double M, double e)
{
    double E = e < 0.8 ? M : ORB_PI;
    for (int i = 0; i < 20; i++) {
        double d = (E - e * sin(E) - M) / (1.0 - e * cos(E));
        E -= d;
        if (fabs(d) < 1e-12) break;
    }
    return E;
}

// Position (km) in the TEME frame at unix time t. 0 if the orbit has decayed.
static int orb_propagate(OrbSat *s, double unixT, OrbVec *r)
{
    const OrbElements *e = &s->e;
    double t = (unixT - e->epoch) / 60.0;           // minutes since epoch

    if (s->deep) {
        double M = fmod(e->mo + s->mdot * t, ORB_TWOPI);
        double argp = e->argp + s->argpdot * t, node = e->raan + s->nodedot * t;
        double E = orb_kepler(M, e->ecc);
        double a = s->a * ORB_RE;
        double px = a * (cos(E) - e->ecc), py = a * sqrt(1.0 - e->ecc * e->ecc) * sin(E);
        double co = cos(argp), so = sin(argp), cn = cos(node), sn = sin(node), ci = cos(e->incl), si = sin(e->incl);
        r->x = (cn * co - sn * so * ci) * px + (-cn * so - sn * co * ci) * py;
        r->y = (sn * co + cn * so * ci) * px + (-sn * so + cn * co * ci) * py;
        r->z = (so * si) * px + (co * si) * py;
        return 1;
    }

    double xmdf = e->mo + s->mdot * t;
    double argpdf = e->argp + s->argpdot * t;
    double nodedf = e->raan + s->nodedot * t;
    double argpm = argpdf, mm = xmdf, t2 = t * t;
    double nodem = nodedf + s->nodecf * t2;
    double tempa = 1.0 - s->cc1 * t;
    double tempe = e->bstar * s->cc4 * t;
    double templ = s->t2cof * t2;
    if (!s->isimp) {
        double delomg = s->omgcof * t;
        double delmtemp = 1.0 + s->eta * cos(xmdf);
        double delm = s->xmcof * (delmtemp * delmtemp * delmtemp - s->delmo);
        double temp = delomg + delm;
        mm = xmdf + temp;
        argpm = argpdf - temp;
        double t3 = t2 * t, t4 = t3 * t;
        tempa = tempa - s->d2 * t2 - s->d3 * t3 - s->d4 * t4;
        tempe = tempe + e->bstar * s->cc5 * (sin(mm) - s->sinmao);
        templ = templ + s->t3cof * t3 + t4 * (s->t4cof + t * s->t5cof);
    }
    double nm = s->no, em = e->ecc, inclm = e->incl;
    if (nm <= 0.0) { s->error = 2; return 0; }
    double am = pow(ORB_XKE / nm, 2.0 / 3.0) * tempa * tempa;
    nm = ORB_XKE / pow(am, 1.5);
    em = em - tempe;
    if (em >= 1.0 || em < -0.001) { s->error = 1; return 0; }
    if (em < 1.0e-6) em = 1.0e-6;
    mm = mm + s->no * templ;
    double xlm = mm + argpm + nodem;
    nodem = fmod(nodem, ORB_TWOPI);
    argpm = fmod(argpm, ORB_TWOPI);
    xlm = fmod(xlm, ORB_TWOPI);
    mm = fmod(xlm - argpm - nodem, ORB_TWOPI);
    double sinip = sin(inclm), cosip = cos(inclm);

    double axnl = em * cos(argpm);
    double temp = 1.0 / (am * (1.0 - em * em));
    double aynl = em * sin(argpm) + temp * s->aycof;
    double xl = mm + argpm + nodem + temp * s->xlcof * axnl;
    double u = fmod(xl - nodem, ORB_TWOPI);
    double eo1 = u, tem5 = 9999.9, sineo1 = 0, coseo1 = 0;
    for (int ktr = 1; fabs(tem5) >= 1.0e-12 && ktr <= 10; ktr++) {
        sineo1 = sin(eo1);
        coseo1 = cos(eo1);
        tem5 = 1.0 - coseo1 * axnl - sineo1 * aynl;
        tem5 = (u - aynl * coseo1 + axnl * sineo1 - eo1) / tem5;
        if (fabs(tem5) >= 0.95) tem5 = tem5 > 0.0 ? 0.95 : -0.95;
        eo1 += tem5;
    }
    double ecose = axnl * coseo1 + aynl * sineo1;
    double esine = axnl * sineo1 - aynl * coseo1;
    double el2 = axnl * axnl + aynl * aynl;
    double pl = am * (1.0 - el2);
    if (pl < 0.0) { s->error = 4; return 0; }
    double rl = am * (1.0 - ecose);
    double betal = sqrt(1.0 - el2);
    temp = esine / (1.0 + betal);
    double sinu = am / rl * (sineo1 - aynl - axnl * temp);
    double cosu = am / rl * (coseo1 - axnl + aynl * temp);
    double su = atan2(sinu, cosu);
    double sin2u = (cosu + cosu) * sinu, cos2u = 1.0 - 2.0 * sinu * sinu;
    temp = 1.0 / pl;
    double temp1 = 0.5 * ORB_J2 * temp, temp2 = temp1 * temp;
    double mrt = rl * (1.0 - 1.5 * temp2 * betal * s->con41) + 0.5 * temp1 * s->x1mth2 * cos2u;
    su = su - 0.25 * temp2 * s->x7thm1 * sin2u;
    double xnode = nodem + 1.5 * temp2 * cosip * sin2u;
    double xinc = inclm + 1.5 * temp2 * cosip * sinip * cos2u;
    double sinsu = sin(su), cossu = cos(su), snod = sin(xnode), cnod = cos(xnode);
    double sini = sin(xinc), cosi = cos(xinc);
    double xmx = -snod * cosi, xmy = cnod * cosi;
    double ux = xmx * sinsu + cnod * cossu, uy = xmy * sinsu + snod * cossu, uz = sini * sinsu;
    r->x = mrt * ux * ORB_RE;
    r->y = mrt * uy * ORB_RE;
    r->z = mrt * uz * ORB_RE;
    if (mrt < 1.0) { s->error = 6; return 0; }
    return 1;
}

// ----------------------------------------------------- Earth and Sun ---

// Greenwich mean sidereal time (radians) at unix time t (UT1 taken as UTC).
static double orb_gmst(double unixT)
{
    double jd = unixT / 86400.0 + 2440587.5;
    double tut1 = (jd - 2451545.0) / 36525.0;
    double temp = -6.2e-6 * tut1 * tut1 * tut1 + 0.093104 * tut1 * tut1 +
                  (876600.0 * 3600.0 + 8640184.812866) * tut1 + 67310.54841;
    temp = fmod(temp * ORB_DEG / 240.0, ORB_TWOPI);
    return temp < 0 ? temp + ORB_TWOPI : temp;
}

static OrbVec orb_teme_to_ecef(OrbVec r, double gmst)
{
    double c = cos(gmst), s = sin(gmst);
    OrbVec o = { c * r.x + s * r.y, -s * r.x + c * r.y, r.z };
    return o;
}

// ECEF (km) to geodetic latitude/longitude (radians) and height (km), WGS-84.
static void orb_geodetic(OrbVec p, double *lat, double *lon, double *h)
{
    const double a = 6378.137, f = 1.0 / 298.257223563, e2 = f * (2.0 - f);
    *lon = atan2(p.y, p.x);
    double r = sqrt(p.x * p.x + p.y * p.y);
    double la = atan2(p.z, r * (1.0 - e2)), N = a, hh = 0;
    for (int i = 0; i < 6; i++) {
        double s = sin(la);
        N = a / sqrt(1.0 - e2 * s * s);
        hh = r / cos(la) - N;
        la = atan2(p.z, r * (1.0 - e2 * N / (N + hh)));
    }
    *lat = la;
    *h = hh;
}

static OrbVec orb_observer_ecef(double lat, double lon, double hKm)
{
    const double a = 6378.137, f = 1.0 / 298.257223563, e2 = f * (2.0 - f);
    double s = sin(lat), N = a / sqrt(1.0 - e2 * s * s);
    OrbVec o = { (N + hKm) * cos(lat) * cos(lon), (N + hKm) * cos(lat) * sin(lon), (N * (1.0 - e2) + hKm) * s };
    return o;
}

// Azimuth (from north, clockwise) and elevation (radians) of an ECEF point from an observer.
static void orb_look(double lat, double lon, OrbVec obs, OrbVec sat, double *az, double *el, double *range)
{
    double rx = sat.x - obs.x, ry = sat.y - obs.y, rz = sat.z - obs.z;
    double sl = sin(lat), cl = cos(lat), so = sin(lon), co = cos(lon);
    double south = sl * co * rx + sl * so * ry - cl * rz;
    double east = -so * rx + co * ry;
    double zen = cl * co * rx + cl * so * ry + sl * rz;
    double rg = sqrt(rx * rx + ry * ry + rz * rz);
    *el = asin(zen / rg);
    double a = atan2(east, -south);
    *az = a < 0 ? a + ORB_TWOPI : a;
    if (range) *range = rg;
}

// The Sun's direction (unit vector, TEME ~ true-of-date) at unix time t.
static OrbVec orb_sun(double unixT)
{
    double n = unixT / 86400.0 + 2440587.5 - 2451545.0;
    double L = fmod(280.460 + 0.9856474 * n, 360.0) * ORB_DEG;
    double g = fmod(357.528 + 0.9856003 * n, 360.0) * ORB_DEG;
    double lam = L + (1.915 * sin(g) + 0.020 * sin(2 * g)) * ORB_DEG;
    double eps = (23.439 - 0.0000004 * n) * ORB_DEG;
    OrbVec s = { cos(lam), cos(eps) * sin(lam), sin(eps) * sin(lam) };
    return s;
}

// The point on Earth with the Sun overhead (radians).
static void orb_subsolar(double unixT, double *lat, double *lon)
{
    OrbVec s = orb_sun(unixT);
    *lat = asin(s.z);
    double l = atan2(s.y, s.x) - orb_gmst(unixT);
    l = fmod(l + ORB_PI, ORB_TWOPI);
    if (l < 0) l += ORB_TWOPI;
    *lon = l - ORB_PI;
}

// Is a TEME position in sunlight? (cylindrical Earth shadow)
static int orb_sunlit(OrbVec r, OrbVec sun)
{
    double d = r.x * sun.x + r.y * sun.y + r.z * sun.z;
    if (d > 0) return 1;
    double px = r.x - d * sun.x, py = r.y - d * sun.y, pz = r.z - d * sun.z;
    return sqrt(px * px + py * py + pz * pz) > ORB_RE;
}

// The Sun's elevation (radians) for an observer.
static double orb_sun_elevation(double lat, double lon, double unixT)
{
    double sl, so;
    orb_subsolar(unixT, &sl, &so);
    return asin(sin(lat) * sin(sl) + cos(lat) * cos(sl) * cos(lon - so));
}

// Where a satellite is: latitude, longitude (radians), altitude (km), and its TEME position.
static int orb_where(OrbSat *s, double unixT, double *lat, double *lon, double *alt, OrbVec *teme)
{
    OrbVec r;
    if (!orb_propagate(s, unixT, &r)) return 0;
    OrbVec p = orb_teme_to_ecef(r, orb_gmst(unixT));
    orb_geodetic(p, lat, lon, alt);
    if (teme) *teme = r;
    return 1;
}

// ------------------------------------------------------------ passes ---

typedef struct {
    double aos, tmax, los;         // unix seconds: rise, highest, set
    double azAos, azMax, azLos;    // radians from north
    double maxEl;                  // radians
    int    visible;                // sunlit while the observer is in darkness, above 10 degrees
} OrbPass;

static double orb_elevation_at(OrbSat *s, double lat, double lon, OrbVec obs, double t, double *az)
{
    OrbVec r;
    if (!orb_propagate(s, t, &r)) return -ORB_PI;
    double a, el;
    orb_look(lat, lon, obs, orb_teme_to_ecef(r, orb_gmst(t)), &a, &el, NULL);
    if (az) *az = a;
    return el;
}

// The next passes over an observer (lat/lon radians, height km), from t0 for
// `hours`, rising above the horizon and peaking at least minElDeg. Returns count.
static int orb_passes(OrbSat *s, double lat, double lon, double hKm, double t0, double hours, double minElDeg,
                      OrbPass *out, int max)
{
    if (s->deep) return 0;                        // high orbits don't "pass"
    OrbVec obs = orb_observer_ecef(lat, lon, hKm);
    int n = 0;
    const double step = 20.0;
    double prev = orb_elevation_at(s, lat, lon, obs, t0, NULL), tEnd = t0 + hours * 3600.0;
    double t = t0;
    int inPass = prev > 0;
    double aos = t0;
    while (t < tEnd && n < max) {
        double tn = t + step;
        double el = orb_elevation_at(s, lat, lon, obs, tn, NULL);
        if (!inPass && el > 0 && prev <= 0) {
            // Rising: pin it down to a second.
            double lo = t, hi = tn;
            while (hi - lo > 1.0) {
                double mid = (lo + hi) / 2;
                if (orb_elevation_at(s, lat, lon, obs, mid, NULL) > 0) hi = mid; else lo = mid;
            }
            aos = hi;
            inPass = 1;
        } else if (inPass && el <= 0 && prev > 0) {
            double lo = t, hi = tn;
            while (hi - lo > 1.0) {
                double mid = (lo + hi) / 2;
                if (orb_elevation_at(s, lat, lon, obs, mid, NULL) > 0) lo = mid; else hi = mid;
            }
            double los = lo;
            // The peak: sample, then golden-section.
            double bestT = aos, bestEl = -1;
            for (double u = aos; u <= los; u += 10.0) {
                double e2 = orb_elevation_at(s, lat, lon, obs, u, NULL);
                if (e2 > bestEl) { bestEl = e2; bestT = u; }
            }
            double a = bestT - 10, b = bestT + 10;
            for (int k = 0; k < 30; k++) {
                double m1 = b - (b - a) * 0.618, m2 = a + (b - a) * 0.618;
                if (orb_elevation_at(s, lat, lon, obs, m1, NULL) > orb_elevation_at(s, lat, lon, obs, m2, NULL)) b = m2;
                else a = m1;
            }
            bestT = (a + b) / 2;
            OrbPass p;
            memset(&p, 0, sizeof p);
            p.aos = aos;
            p.los = los;
            p.tmax = bestT;
            p.maxEl = orb_elevation_at(s, lat, lon, obs, bestT, &p.azMax);
            orb_elevation_at(s, lat, lon, obs, aos, &p.azAos);
            orb_elevation_at(s, lat, lon, obs, los, &p.azLos);
            if (p.maxEl >= minElDeg * ORB_DEG && aos > t0) {
                // Visible: the satellite in sunlight while the sky is dark.
                for (double u = aos; u <= los && !p.visible; u += 15.0) {
                    OrbVec r;
                    if (!orb_propagate(s, u, &r)) break;
                    if (orb_elevation_at(s, lat, lon, obs, u, NULL) > 10 * ORB_DEG &&
                        orb_sun_elevation(lat, lon, u) < -6 * ORB_DEG && orb_sunlit(r, orb_sun(u)))
                        p.visible = 1;
                }
                out[n++] = p;
            }
            inPass = 0;
        }
        prev = el;
        t = tn;
    }
    return n;
}

// A compass point for an azimuth (radians).
static const char *orb_compass(double az)
{
    static const char *const P[16] = { "N", "NNE", "NE", "ENE", "E", "ESE", "SE", "SSE",
                                       "S", "SSW", "SW", "WSW", "W", "WNW", "NW", "NNW" };
    int i = (int)floor(az / ORB_TWOPI * 16.0 + 0.5) % 16;
    return P[i < 0 ? i + 16 : i];
}
