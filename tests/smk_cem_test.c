/* SMK-37 CEM / LADDER filter (firmware/src/smk_cem.h; smk37-devtools cem-filter-spec.md §5b3), on the host, against the
 * float reference the owner approved by ear (sloop-artifacts/cem-proto-3 / -4: CEM r2 and LADDER r2, round 4's RES
 * scaled against WARMTH): the null at a range of settings, low cutoffs, self-oscillation in tune, decay to silence,
 * the 12 dB tap, the 4-pole high-pass by pole mixing, extremes bounded (no overflow), the DRIFT walk bounded.
 *   cc -Ibuild/gen -o smk_cem_test tests/smk_cem_test.c -lm && ./smk_cem_test */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static inline int32_t clamp(int32_t v, int32_t lo, int32_t hi) { return v < lo ? lo : v > hi ? hi : v; }
#include "felucca_tables.h"                        /* TANH_Q15 (build/gen) */
static inline uint32_t noise32(int32_t *st)
{
    uint32_t s = (uint32_t)*st;
    s ^= s << 13, s ^= s >> 17, s ^= s << 5;
    *st = (int32_t)s;
    return s;
}
#include "../firmware/src/smk_cem.h"

static int fails, verbose;                         /* (VERBOSE=1: every case) */
static void check(const char *what, int ok)
{
    printf("%-100s %s\n", what, ok ? "ok" : "FAIL");
    fails += !ok;
}

#undef FS
#define FS 44100.0
#define CTL 32
#define PI 3.14159265358979
static double cut_hz(double idx) { return 30.0 * pow(16000.0 / 30.0, idx / 127.0); }

/* the float reference (cemfix.c run_f) */
typedef struct { double s[4]; } flt_t;
typedef struct { double G, k, kl, hot, bias, pre; int lad; } cf_t;
static void coef_f(cf_t *c, double idx, int res, int lad, double hot, double bias, double pre)
{
    double f = cut_hz(fmax(0, fmin(127, idx))), g;
    if (f > 0.45 * FS) f = 0.45 * FS;
    g = tan(PI * f / FS);
    c->G = g / (1 + g), c->k = 4.3 * res / 127.0, c->lad = lad, c->hot = hot, c->bias = bias, c->pre = pre;
    c->kl = lad ? c->k : c->k / pow(1 - tanh(bias) * tanh(bias), 4);
}
static double run_f(flt_t *z, const cf_t *c, double x, double *hp, int tap2)
{
    double G = c->G, b = 1 - G, k = c->k, S, u, y = 0, in, ys[5];
    int i;
    S = G * G * G * b * z->s[0] + G * G * b * z->s[1] + G * b * z->s[2] + b * z->s[3];
    in = x * c->pre * c->hot * (c->lad ? 1 : 1 + 0.7 * k);
    if (c->lad) S = tanh(S * 1.2) / 1.2;
    u = (in - c->kl * S) / (1 + c->kl * G * G * G * G);
    u = c->lad ? tanh(1.8 * u) / 1.8 : tanh(u);
    ys[0] = u;
    for (i = 0; i < 4; i++) {
        double xi = !c->lad ? tanh(u + c->bias) - tanh(c->bias) : i ? tanh(u) : u;
        double v = G * (xi - z->s[i]);
        y = v + z->s[i];
        z->s[i] = y + v;
        u = y;
        ys[i + 1] = y;
    }
    if (hp) *hp = (ys[0] - 4 * ys[1] + 6 * ys[2] - 4 * ys[3] + ys[4]) / (c->hot * 0.8 * c->pre);
    y = (tap2 ? ys[2] : y) / (c->hot * 0.8 * c->pre);
    return c->lad ? y * (1 + 0.5 * k) : y;
}

#define NMAX (44100 * 4)
static double xin[NMAX], yf[NMAX], yx[NMAX];
static double blep(double t, double dt)
{
    if (t < dt) { t /= dt; return t + t - t * t - 1; }
    if (t > 1 - dt) { t = (t - 1) / dt; return t * t + t + t + 1; }
    return 0;
}
static double saw(double *ph, double hz)
{
    double dt = hz / FS, y = 2 * *ph - 1 - blep(*ph, dt);
    *ph += dt;
    if (*ph >= 1) *ph -= 1;
    return y;
}
static int16_t q15(double v) { v = fmax(-1, fmin(32767.0 / 32768, v)); return (int16_t)lrint(v * 32768); }
/* both over xin[0..n): drv 0..127 (x1..x4), warmth 0..100, pre (as ANALOG: 0.6) */
static void both(int n, int lad, double idx, int res, int drv, int warm, int hp, double pre)
{
    flt_t zf; int32_t zx[4] = {0}; cf_t cf; cem_t cx; int i;
    memset(&zf, 0, sizeof zf);
    cem_warm = (uint8_t)warm;
    for (i = 0; i < n; i++) {
        if (i % CTL == 0) {
            coef_f(&cf, idx, res, lad, 1 + 3.0 * drv / 127, 0.5 * warm / 100, pre);
            cem_coef(&cx, (int32_t)lrint(idx * 256), res, (uint32_t)lad, CEM_ONE + drv * ((3 << 24) / 127), (int32_t)lrint(pre * CEM_ONE));
        }
        int16_t s = q15(xin[i]);
        double h; int32_t hx, b;
        double a = run_f(&zf, &cf, s / 32768.0, &h, cem_slope);
        b = cem_tick(zx, &cx, (int32_t)s << 9, &hx);
        yf[i] = hp ? h : a, yx[i] = (hp ? hx : b) / 16777216.0;
    }
}
static double null_db(int i0, int i1)
{
    double e = 0, p = 0; int i;
    for (i = i0; i < i1; i++) e += (yx[i] - yf[i]) * (yx[i] - yf[i]), p += yf[i] * yf[i];
    return 10 * log10(e / (p + 1e-30) + 1e-30);
}
static double freq_of(const double *y, int a, int b)
{
    double t0 = -1, t1 = 0; int i, nc = 0;
    for (i = a + 1; i < b; i++)
        if (y[i - 1] < 0 && y[i] >= 0) {
            double t = i - 1 + y[i - 1] / (y[i - 1] - y[i]);
            if (t0 < 0) t0 = t; else t1 = t, nc++;
        }
    return nc ? nc * FS / (t1 - t0) : 0;
}
static double gain_db(double idx, double hz, int res, int hp)
{
    int n = 22050, i; double e = 0, ei = 0;
    for (i = 0; i < n; i++) xin[i] = 0.05 * sin(2 * PI * hz * i / FS);
    both(n, 0, idx, res, 0, 0, hp, 1.0);
    for (i = n / 2; i < n; i++) e += yx[i] * yx[i], ei += xin[i] * xin[i];
    return 10 * log10(e / ei);
}

int main(void)
{
    int i, t, n;
    char what[160];
    double worst;
    verbose = getenv("VERBOSE") != 0;
    {   /* null against the float reference: a saw at C3 */
        static const int cs[][4] = {{20, 30, 64, 36}, {55, 30, 64, 36}, {90, 30, 64, 36}, {55, 90, 64, 36}, {55, 120, 64, 36},
                                    {55, 30, 0, 36}, {55, 30, 127, 36}, {55, 30, 64, 0}, {55, 30, 64, 100}, {110, 60, 64, 36}};
        double ph = 0;
        n = 44100;
        for (i = 0; i < n; i++) xin[i] = 0.6 * saw(&ph, 130.81);
        for (t = 0; t < 2; t++) {
            worst = -999;
            for (i = 0; i < 10; i++) {
                both(n, t, cs[i][0], cs[i][1], cs[i][2], cs[i][3], 0, 0.6);
                worst = fmax(worst, null_db(4410, n));
                if (verbose)
                    printf("  %s CUT %d RES %d DRV %d WARMTH %d: %.1f dB\n", t ?"LADDER" : "CEM", cs[i][0], cs[i][1], cs[i][2], cs[i][3], null_db(4410, n));
            }
            snprintf(what, sizeof what, "%s: fixed point = the float reference, CUT / RES / DRV / WARMTH (worst null %.1f dB, < %d)",
                     t ? "LADDER" : "CEM", worst, t ? -50 : -55);   /* (LADDER: RES 120, next to self-oscillation) */
            check(what, worst < (t ? -50 : -55));
        }
    }
    {   /* low cutoffs: the gain at the cutoff, as float (CEM_G, not SVF_G's steps) */
        worst = 0;
        for (i = 0; i <= 30; i += 6) {
            int nn = 44100, j; double ef = 0, ex = 0;
            for (j = 0; j < nn; j++) xin[j] = 0.05 * sin(2 * PI * cut_hz(i) * j / FS);
            both(nn, 0, i, 0, 0, 0, 0, 1.0);
            for (j = nn / 2; j < nn; j++) ef += yf[j] * yf[j], ex += yx[j] * yx[j];
            worst = fmax(worst, fabs(10 * log10(ex / ef)));
        }
        snprintf(what, sizeof what, "low cutoffs (30 .. 132 Hz): the gain at the cutoff as float (worst %.2f dB, < 0.2)", worst);
        check(what, worst < 0.2);
    }
    for (t = 0; t < 2; t++) {   /* self-oscillation at RES 127: as float at every WARMTH (high WARMTH sharpens it at high
                                 * cutoffs: +51 cents at 100, CUT 90, in float too: the voicing); in tune at the default */
        double wc = 0, wd = 0;
        static const int WS[4] = {0, CEM_WARM_DEF, 70, 100};
        int w, wi;
        for (wi = 0; wi < 4 && (w = WS[wi]) >= 0; wi++)
            for (i = 30; i <= 90; i += 20) {
                n = 44100 * 2;
                memset(xin, 0, (size_t)n * sizeof *xin);
                xin[0] = 0.5;
                both(n, t, i, 127, 64, w, 0, 0.6);
                double fx = freq_of(yx, n / 2, n), c = fx > 0 ? 1200 * log2(fx / cut_hz(i)) : 9999, f2 = freq_of(yf, n / 2, n);
                wd = fmax(wd, fabs(c - (f2 > 0 ? 1200 * log2(f2 / cut_hz(i)) : -9999)));
                if (w == CEM_WARM_DEF)
                    wc = fmax(wc, fabs(c));
                if (verbose) {
                    double ff = freq_of(yf, n / 2, n);
                    printf("  %s WARMTH %d CUT %d: fixed %+.1f, float %+.1f cents\n", t ?"LADDER" : "CEM", w, i, c, ff > 0 ? 1200 * log2(ff / cut_hz(i)) : 9999.0);
                }
            }
        snprintf(what, sizeof what, "%s: self-oscillates at RES 127, CUT 30 .. 90: as float, WARMTH 0 .. 100 (%.2f cents, < 1); in tune at 36 (%.1f, < 10)",
                 t ? "LADDER" : "CEM", wd, wc);
        check(what, wd < 1 && wc < 10);
    }
    for (t = 0; t < 2; t++) {   /* below self-oscillation: silence after the note, exactly */
        double ph = 0, m = 0;
        n = 44100 * 3;
        for (i = 0; i < n; i++) xin[i] = i < 22050 ? 0.6 * saw(&ph, 65.4) : 0;
        both(n, t, 50, 100, 64, 36, 0, 0.6);
        for (i = n - 22050; i < n; i++) m = fmax(m, fabs(yx[i]));
        snprintf(what, sizeof what, "%s: RES 100, the note gone: silence (no limit cycle; largest %.5f)", t ? "LADDER" : "CEM", m);
        check(what, m < 1e-5);
    }
    {   /* the 4-pole high-pass by pole mixing, as an ideal one (the track FILTER's right half) */
        worst = 0;
        for (double m = 0.25; m <= 8.01; m *= 2) {
            double id = 20 * log10(pow(m, 4) / pow(1 + m * m, 2)) + 20 * log10(1 / 0.8);
            worst = fmax(worst, fabs(gain_db(64, cut_hz(64) * m, 0, 1) - id));
        }
        snprintf(what, sizeof what, "high-pass by pole mixing: an ideal 4-pole high-pass, fc / 4 .. 8 fc (worst %.2f dB, < 0.5)", worst);
        check(what, worst < 0.5);
        double pk = gain_db(64, cut_hz(64), 60, 1) - gain_db(64, cut_hz(64), 0, 1);
        snprintf(what, sizeof what, "high-pass: RES peaks it at the cutoff (+%.1f dB at RES 60)", pk);
        check(what, pk > 6);
    }
    {   /* SLOPE 12: stage 2's tap, -12 dB an octave above the cutoff (24: -24) */
        double a24, b24, a12, b12;
        a24 = gain_db(40, cut_hz(40) * 4, 0, 0), b24 = gain_db(40, cut_hz(40) * 8, 0, 0);
        cem_slope = 1;
        a12 = gain_db(40, cut_hz(40) * 4, 0, 0), b12 = gain_db(40, cut_hz(40) * 8, 0, 0);
        cem_slope = 0;
        snprintf(what, sizeof what, "SLOPE: 24 dB (%.1f dB / oct) and 12 dB (%.1f dB / oct) above the cutoff", b24 - a24, b12 - a12);
        check(what, b24 - a24 < -20 && b24 - a24 > -26 && b12 - a12 < -9 && b12 - a12 > -13);
    }
    {   /* extremes: a full-scale square, RES 127, DRV 127, WARMTH 100, every cutoff: bounded, no overflow */
        double m = 0;
        for (t = 0; t < 2; t++)
            for (i = 0; i <= 127; i += 9) {
                int j;
                n = 8820;
                for (j = 0; j < n; j++) xin[j] = (j / 50) & 1 ? 1.0 : -1.0;
                both(n, t, i, 127, 127, 100, 0, 0.6);
                for (j = 0; j < n; j++) m = fmax(m, fabs(yx[j]) * (isfinite(yf[j]) ? 1 : 1e9));
                both(n, t, i, 127, 127, 100, 1, 0.25);
                for (j = 0; j < n; j++) m = fmax(m, fabs(yx[j]));
            }
        snprintf(what, sizeof what, "extremes (a full-scale square, RES / DRV / WARMTH at the top, every CUT): bounded (peak %.2f, < 8)", m);
        check(what, m < 8);
    }
    {   /* DRIFT: bounded walks */
        int32_t dp = 0, dc = 0, st = 0x1234567, mp = 0, mc = 0;
        for (i = 0; i < 200000; i++) {
            cem_drift_step(&dp, &dc, &st);
            mp = dp > mp ? dp : -dp > mp ? -dp : mp, mc = dc > mc ? dc : -dc > mc ? -dc : mc;
        }
        snprintf(what, sizeof what, "DRIFT: pitch within +-4 cents (%.2f), cutoff within +-0.8 steps (%.2f), and it moves",
                 mp / 65536.0, mc / 65536.0);
        check(what, mp <= 4 << 16 && mc <= 52429 && mp > 65536 && mc > 13107);
        uint32_t inc = 100000000u, u = cem_detune(inc, 4 << 16);
        double c = 1200 * log2((double)u / inc);
        snprintf(what, sizeof what, "DRIFT: +4 cents on an oscillator is %.3f cents", c);
        check(what, fabs(c - 4) < 0.01);
    }
    printf(fails ? "SMK CEM TEST FAILED (%d)\n" : "smk_cem: all ok\n", fails);
    return fails != 0;
}
