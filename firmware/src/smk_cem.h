/* SPDX-License-Identifier: GPL-3.0-only */
/* SMK-37, the vocoder builds: the CEM / LADDER filter and RING (smk37-devtools cem-filter-spec.md, signed off by the
 * owner 2026-11-21 after four rounds of renders: sloop-artifacts/cem-proto-1 .. -4; this is cem-proto-4's cemfix.c, the
 * fixed point checked there against the float reference: null -58 .. -68 dB, self-oscillation in tune within 0.2 cents
 * of it).
 *   Four trapezoidal (zero-delay-feedback) one-pole low-passes sharing one cutoff, resonance as feedback from the last
 *   stage solved without a unit delay (Zavalishin), up to self-oscillation (from RES ~118). CEM: each stage saturates
 *   softly and asymmetrically (tanh(x + b) - tanh(b), b = WARMTH: even harmonics), driven hot, the bass compensated as
 *   RES rises (1 + 0.7 k at the input); LADDER: per-stage tanh, its feedback saturating too, no bass compensation.
 *   The loop's k divided by the stages' small-signal gain (1 - tanh(b)^2)^4, so RES means the same at every WARMTH.
 *   Signals and states Q24 (1.0 = 2^24), one int64 multiply a tap (as smk_harm.h's biquad); tanh from TANH_Q15 (dsp.c
 *   softclip's table, 0 .. 2) and a tail to 4.0; the cutoff from CEM_G (G = g / (1 + g), Q31: SVF_G's Q12 g is 8 at
 *   30 Hz, steps of ~12 % down there). Coefficients once a block.
 * Where it runs: ANALOG's EDIT > FILTER > TYPE CEM / LADDER (eng_analog.c, per voice), each track's FILTER by HOME >
 * TRACK FILTER (fx.c tflt: a synth part, an input track, the vocoder's output, the drums), with HOME > FILTER's SLOPE,
 * WARMTH and DRIFT for all of them. RING: fader bank RING (the last one), osc 1 x osc 2 in ANALOG, an input track's
 * channel x a sine at its last note (voice.c). */
#define SMK_CEM 1

/* HOME > FILTER (unit settings, project.c vocset) */
static uint8_t cem_slope;                /* SLOPE: 0 24 dB, 1 12 dB (stage 2's output; the feedback from stage 4) */
#define CEM_WARM_DEF 36u                 /* WARMTH 0 .. 100 = bias 0 .. 0.5; 36 = 0.18, the voicing chosen (round 2) */
static uint8_t cem_warm = CEM_WARM_DEF;
#define CEM_DRIFT_DEF 50u                /* DRIFT 0 .. 100, 0 off; 50 = +-4 cents, +-0.8 cutoff steps a voice (round 2) */
static uint8_t cem_drift = CEM_DRIFT_DEF;
enum { TFT_SVF, TFT_CEM, TFT_LADDER, TFT_N };
static uint8_t cem_tt[4];                /* HOME > TRACK FILTER: T1, T2, T3, DRUMS (the track's FILTER, bank B) */
static int16_t smk_ring[4];              /* FADER BANK RING: T1 .. T3, 0 off .. 127 (kept with the project, vocset) */

static const int32_t CEM_G[128] = {
    4579688, 4811289, 5054575, 5310133, 5578577, 5860556, 6156747, 6467862,
    6794649, 7137892, 7498414, 7877078, 8274792, 8692504, 9131213, 9591966,
    10075858, 10584042, 11117723, 11678169, 12266706, 12884725, 13533685, 14215116,
    14930619, 15681874, 16470641, 17298762, 18168169, 19080884, 20039024, 21044808,
    22100556, 23208697, 24371774, 25592446, 26873494, 28217828, 29628489, 31108655,
    32661650, 34290943, 36000158, 37793079, 39673657, 41646012, 43714443, 45883431,
    48157648, 50541961, 53041437, 55661354, 58407202, 61284688, 64299750, 67458551,
    70767496, 74233229, 77862641, 81662875, 85641332, 89805670, 94163812, 98723948,
    103494537, 108484308, 113702263, 119157677, 124860096, 130819338, 137045492, 143548911,
    150340211, 157430269, 164830209, 172551407, 180605472, 189004246, 197759791, 206884381,
    216390489, 226290779, 236598096, 247325452, 258486020, 270093120, 282160217, 294700911,
    307728937, 321258163, 335302597, 349876394, 364993876, 380669553, 396918161, 413754702,
    431194504, 449253292, 467947284, 487293295, 507308878, 528012484, 549423663, 571563293,
    594453861, 618119794, 642587852, 667887588, 694051900, 721117683, 749126599, 778126005,
    808170054, 839321018, 871650895, 905243354, 940196121, 976623916, 1014662114, 1054471341,
    1096243294, 1140208224, 1186644627, 1235892007, 1288367887, 1344590867, 1405212396, 1471061427};

#define CEM_ONE (1 << 24)
#define CEM_M31(a, b) ((int32_t)(((int64_t)(a) * (b)) >> 31))
#define CEM_M30(a, b) ((int32_t)(((int64_t)(a) * (b)) >> 30))
/* tanh, Q24 in and out: the table to 2.0, then straight lines through tanh(2), tanh(2.5), tanh(3), tanh(4), flat on */
static inline __attribute__((always_inline)) int32_t cem_th(int32_t x)
{
    int32_t a = x < 0 ? -x : x, y;
    if (a < (2 << 24)) {
        int32_t i = a >> 17, f = (a >> 2) & 0x7FFF;
        y = (TANH_Q15[i] + (((TANH_Q15[i + 1] - TANH_Q15[i]) * f) >> 15)) << 9;
    } else if (a < (5 << 23)) {
        y = 16173699 + (int32_t)(((int64_t)(16552641 - 16173699) * (a - (2 << 24))) >> 23);
    } else if (a < (3 << 24)) {
        y = 16552641 + (int32_t)(((int64_t)(16694249 - 16552641) * (a - (5 << 23))) >> 23);
    } else if (a < (4 << 24)) {
        y = 16694249 + (int32_t)(((int64_t)(16765964 - 16694249) * (a - (3 << 24))) >> 24);
    } else {
        y = 16765964;
    }
    return x < 0 ? -y : y;
}

/* WARMTH's values, worked out when it changes: the bias b, tanh(b), and 1 / (1 - tanh(b)^2)^4 (Q28) for the loop's k */
static uint8_t cem_wseen = 0xFFu;
static int32_t cem_tb, cem_thb, cem_kw;
static void cem_warm_calc(void)                  /* (both cores may run it at once: the same values; seen set last) */
{
    uint8_t w = cem_warm;
    int32_t sl, s2, s4, tb = (int32_t)(w > 100u ? 100u : w) * (CEM_ONE / 200), thb = cem_th(tb);   /* (0.5 at 100) */
    sl = (1 << 30) - (int32_t)(((int64_t)thb * thb) >> 18);
    s2 = CEM_M30(sl, sl), s4 = CEM_M30(s2, s2);
    cem_kw = (int32_t)(((int64_t)1 << 58) / s4);                /* (up to 2.6 at WARMTH 100) */
    cem_tb = tb, cem_thb = thb;
    cem_wseen = w;
}

typedef struct { int32_t G, c[4], inm, kinv, outm, hpm, tb, thb; uint8_t lad, tap2; } cem_t;
/* a block's coefficients: cut 0 .. 127 << 8 (the CUTOFF_HZ index), res 0 .. 127, lad (LADDER), hot (the drive, Q24:
 * x 1 .. x 4), pre (the input's scale into the filter, Q24; the output divided by it again) */
static void cem_coef(cem_t *c, int32_t cut, int32_t res, uint32_t lad, int32_t hot, int32_t pre)
{
    int32_t i, G, G2, G3, G4, b, k, kl;
    int64_t den, inv, h, o;
    if (cem_warm != cem_wseen)
        cem_warm_calc();
    cut = cut < 0 ? 0 : cut > (127 << 8) ? 127 << 8 : cut;
    res = res < 0 ? 0 : res > 127 ? 127 : res;
    i = cut >> 8;
    G = CEM_G[i];
    if (i < 127)
        G += (int32_t)(((int64_t)(CEM_G[i + 1] - G) * (cut & 255)) >> 8);
    b = (int32_t)(2147483647 - G);
    G2 = CEM_M31(G, G), G3 = CEM_M31(G2, G), G4 = CEM_M31(G3, G);
    c->G = G;
    c->c[0] = CEM_M31(G3, b), c->c[1] = CEM_M31(G2, b), c->c[2] = CEM_M31(G, b), c->c[3] = b;
    k = res * (int32_t)(4.3 * 16777216.0 / 127);                 /* Q24 */
    kl = lad ? k : (int32_t)(((int64_t)k * cem_kw) >> 28);
    den = CEM_ONE + (((int64_t)kl * G4) >> 31);                  /* 1 + k G^4, Q24 */
    inv = ((int64_t)1 << 54) / den;                              /* Q30 */
    c->kinv = (int32_t)(((int64_t)kl * inv) >> 27);              /* Q27 (up to 11.3) */
    h = lad ? hot : ((int64_t)hot * (CEM_ONE + (k * 7) / 10)) >> 24;   /* hot (1 + 0.7 k), Q24 */
    h = (h * pre) >> 24;
    c->inm = (int32_t)((h * inv) >> 28);                         /* .. / (1 + k G^4), Q26 */
    o = ((int64_t)1 << 50) / ((((int64_t)hot * 4) / 5 * pre) >> 24);   /* 1 / (0.8 hot pre), Q26 */
    c->hpm = (int32_t)o;
    if (lad)
        o = (o * (CEM_ONE + k / 2)) >> 24;                       /* LADDER: x (1 + 0.5 k) */
    c->outm = (int32_t)o;
    c->tb = cem_tb, c->thb = cem_thb, c->lad = (uint8_t)lad, c->tap2 = cem_slope & 1u;
}
/* a sample: x Q24 in, the low-pass out (Q24); hp: the 4-pole high-pass by pole mixing (u - 4 y1 + 6 y2 - 4 y3 + y4) */
static inline __attribute__((always_inline)) int32_t cem_tick(int32_t *s, const cem_t *c, int32_t x, int32_t *hp)
{
    int32_t S, u, y, v, u0, y1, y2, y3, G = c->G;
    S = CEM_M31(c->c[0], s[0]) + CEM_M31(c->c[1], s[1]) + CEM_M31(c->c[2], s[2]) + CEM_M31(c->c[3], s[3]);
    if (c->lad)
        S = CEM_M31(cem_th(S + S / 5), 1789569707);              /* tanh(1.2 S) / 1.2 */
    u = (int32_t)(((int64_t)x * c->inm) >> 26) - (int32_t)(((int64_t)S * c->kinv) >> 27);
    u = c->lad ? CEM_M31(cem_th(u + (u / 5) * 4), 1193046471)    /* tanh(1.8 u) / 1.8 */
               : cem_th(u);
    u0 = u;
#define CEM_STAGE(n, sat)                                        \
    v = CEM_M31((sat) - s[n], G);                                \
    y = v + s[n];                                                \
    s[n] = y + v;                                                \
    u = y;
    if (!c->lad) {
        CEM_STAGE(0, cem_th(u + c->tb) - c->thb) y1 = y;
        CEM_STAGE(1, cem_th(u + c->tb) - c->thb) y2 = y;
        CEM_STAGE(2, cem_th(u + c->tb) - c->thb) y3 = y;
        CEM_STAGE(3, cem_th(u + c->tb) - c->thb)
    } else {
        CEM_STAGE(0, u) y1 = y;
        CEM_STAGE(1, cem_th(u)) y2 = y;
        CEM_STAGE(2, cem_th(u)) y3 = y;
        CEM_STAGE(3, cem_th(u))
    }
#undef CEM_STAGE
    if (hp)
        *hp = (int32_t)(((int64_t)(u0 - 4 * y1 + 6 * y2 - 4 * y3 + y) * c->hpm) >> 26);
    return (int32_t)(((int64_t)(c->tap2 ? y2 : y) * c->outm) >> 26);
}

/* DRIFT (ANALOG, CEM / LADDER only): a voice's slow random walks, once a block: pitch (Q16 cents, +-4) and cutoff (Q16
 * steps of the cutoff law, +-0.8), scaled by DRIFT (50: as is) where they are used */
static inline void cem_drift_step(int32_t *dp, int32_t *dc, int32_t *nst)
{
    int32_t r1 = (int32_t)(noise32(nst) >> 16) - 32768, r2 = (int32_t)(noise32(nst) >> 16) - 32768;
    *dp = clamp(*dp - (*dp >> 10) + ((r1 * 5243) >> 15), -(4 << 16), 4 << 16);
    *dc = clamp(*dc - (*dc >> 10) + ((r2 * 1311) >> 15), -52429, 52429);
}
/* an oscillator increment moved by c16 (Q16 cents) */
static inline uint32_t cem_detune(uint32_t inc, int32_t c16)
{
    int32_t f = (int32_t)(((int64_t)c16 * 9693) >> 16);         /* Q24: 2^(c / 1200) - 1 ~ c x 0.000577789 */
    return inc + (uint32_t)(int32_t)(((int64_t)inc * f) >> 24);
}
