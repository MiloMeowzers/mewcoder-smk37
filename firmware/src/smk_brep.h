/* SPDX-License-Identifier: GPL-3.0-only */
/* SMK-37: BEAT REPEAT, a sequencer / MIDI effect (smk37-devtools punch-page2-spec.md §3, the owner 2026-11-20; Ableton's
 * Beat Repeat as the reference). Per track, latched on (the punch's page 2, top pads 5-8, or FX CH notes 84-87): at each
 * INTERVAL (shifted by OFFSET, counted from the bar) it fires with CHANCE; firing, it captures the notes the track plays
 * over the next GRID (the sequence's, after FLIP / LOOP BACK, and the keys': whatever reaches trk_note_on) and plays
 * them again every GRID until GATE has passed, each repeat quieter (DECAY) and lower (PITCH; not on the drums). It makes
 * notes, no audio: the stems, the stereo mix and MIDI out (MIDI = SEQ) follow; its cost is the voices the notes take
 * (POLY CAP AUTO bounds FM6). WET / DRY balances the repeats against the track's own notes (0 only the track, 50 both
 * full, 100 only the repeats while they play); MUTE: no repeat notes at all (100 % dry). A HARMONY track (its notes are
 * intervals) and an audio track (its notes play nothing) capture nothing. Block timing: the audio block's start (5.8 ms
 * at most early); the GRIDs under a step land between steps (1/32 ...) as the block allows */
#define BR_EV 16u                                       /* notes a capture holds (the owner: the store kept small) */
enum { BR_IVAL, BR_OFS, BR_CHANCE, BR_GRID, BR_GATE, BR_DECAY, BR_PITCH, BR_WD, BR_NS };
static const uint8_t BR_DEF[BR_NS] = {5, 0, 100, 4, 4, 0, 0, 50};   /* 1 bar, 0/16, 100 %, 1/16, 4/16, 0 %, 0, 50 */
static const uint8_t BR_MAX[BR_NS] = {7, 15, 100, 8, 64, 100, 48, 100};
static const uint8_t BR_IV32[8] = {1, 2, 4, 8, 16, 32, 64, 128};     /* INTERVAL in 1/32 notes: 1/32 .. 4 bars */
static const char *const BR_IVN[8] = {"1/32", "1/16", "1/8", "1/4", "1/2", "1 bar", "2 bars", "4 bars"};
static const uint8_t BR_GRIDN[9] = {4, 6, 8, 12, 16, 24, 32, 48, 64};   /* GRID: 1/N of a bar (6, 12, 24, 48: triplets) */
static uint8_t br_s[NTRK][BR_NS] = {{5, 0, 100, 4, 4, 0, 0, 50}, {5, 0, 100, 4, 4, 0, 0, 50}, {5, 0, 100, 4, 4, 0, 0, 50},
                                    {5, 0, 100, 4, 4, 0, 0, 50}};   /* each track's (project.c smkset br) */
static uint8_t br_var;                                  /* HOME > PUNCH 2 > VARIATION 0..10 (each repeat's grid at random
                                                         * around GRID) */
static uint8_t br_trip = 1u;                            /* .. TRIPLETS: GRID's 1/6, 1/12 .. (Ableton's NO TRIPLETS off) */
static uint8_t br_on, br_mute;                          /* bit k: track k latched on; its repeats muted */
static uint8_t br_self;                                 /* the repeater plays: not captured, not scaled */
typedef struct {
    uint8_t phase, n, k, shift;                         /* 0 idle, 1 capturing, 2 repeating; notes held; the repeat; its
                                                         * transposition down */
    uint16_t vg;                                        /* this repeat's level (Q15, 32768 = 1: DECAY) */
    uint32_t glen;                                      /* this repeat's GRID (clock units) */
    uint64_t cs, end;                                   /* this repeat's start, GATE's end (clock units since PLAY) */
    uint32_t pulse_ms;                                  /* (the LEDs: the last INTERVAL) */
    struct { uint32_t off; uint8_t note, vel; } ev[BR_EV];
} br_t;
static br_t br[NTRK];
static uint32_t br_rs = 0x2545F491u;
static uint32_t br_rand(uint32_t m) { br_rs = br_rs * 1664525u + 1013904223u; return (br_rs >> 8) % (m ? m : 1u); }

static void trk_note_on(track_t *t, uint32_t note, uint32_t vel);
static void trk_note_off(track_t *t, uint32_t note);
static void seq_out_on(const track_t *t, uint32_t note, uint32_t vel);
static void seq_out_off(const track_t *t, uint32_t note);

static uint64_t br_now(void) { return (uint64_t)clk_beat * BEAT_U + clk_pos; }
static uint32_t br_grid_ix(uint32_t gi)                 /* GRID's index, the triplets skipped while they are off */
{
    gi = gi > 8u ? 8u : gi;
    if (!br_trip && (gi == 1u || gi == 3u || gi == 5u || gi == 7u))
        gi--;
    return gi;
}
static uint32_t br_glen(uint32_t k)                     /* a repeat's GRID: VARIATION picks around it */
{
    int32_t gi = (int32_t)br_grid_ix(br_s[k][BR_GRID]), sp = (int32_t)(br_var + 1u) / 2;
    if (sp) {
        gi += (int32_t)br_rand((uint32_t)(2 * sp + 1)) - sp;
        gi = gi < 0 ? 0 : gi > 8 ? 8 : gi;
        gi = (int32_t)br_grid_ix((uint32_t)gi);
    }
    return (uint32_t)((uint64_t)BEAT_U * 4u / BR_GRIDN[gi]);
}
static uint32_t br_note_of(uint32_t k, uint32_t note, uint32_t shift)   /* PITCH: lower each repeat (the drums as they are) */
{
    return k == TRK_DRUM || note < shift ? note : note - shift;
}
static void br_offs(uint32_t k)                         /* this repeat's notes end */
{
    br_t *b = &br[k];
    uint32_t i;
    if (b->phase != 2u || ((br_mute >> k) & 1u))
        return;
    br_self = 1;
    for (i = 0; i < b->n; i++) {
        uint32_t nt = br_note_of(k, b->ev[i].note, b->shift);
        if (k != TRK_DRUM)
            trk_note_off(&trk[k], nt);
        seq_out_off(&trk[k], nt);
    }
    br_self = 0;
}
static void br_stop(uint32_t k)
{
    br_offs(k);
    br[k].phase = 0, br[k].n = 0;
}
static void br_reset(void)                              /* the transport starts, or stops */
{
    uint32_t k;
    for (k = 0; k < NTRK; k++)
        br_stop(k);
}
/* trk_note_on, every note (vel 1..127): the capture, and the track's own notes balanced (DRY); 0: not played */
static uint32_t br_note(track_t *t, uint32_t note, uint32_t vel)
{
    uint32_t k = (uint32_t)(t - trk) % NTRK, wd = br_s[k][BR_WD];
    br_t *b = &br[k];
    if (!((br_on >> k) & 1u))
        return vel;
    if (b->phase == 1u && b->n < BR_EV) {
        uint64_t now = br_now();
        b->ev[b->n].off = now > b->cs ? (uint32_t)(now - b->cs) : 0u;
        b->ev[b->n].note = (uint8_t)note, b->ev[b->n].vel = (uint8_t)vel;
        b->n++;
    }
    if (b->phase == 2u && !((br_mute >> k) & 1u) && wd > 50u) {   /* (WET over 50: the track's own notes go down) */
        vel = vel * (100u - wd) * 2u / 100u;
        return vel;
    }
    return vel;
}
static void br_play(uint32_t k, uint32_t i)            /* capture note i, as this repeat plays it */
{
    br_t *b = &br[k];
    uint32_t wd = br_s[k][BR_WD], v = b->ev[i].vel, nt = br_note_of(k, b->ev[i].note, b->shift);
    v = v * (wd < 50u ? wd * 2u : 100u) / 100u;         /* WET (under 50: the repeats go down) */
    v = v * b->vg >> 15;
    if (!v || ((br_mute >> k) & 1u))
        return;
    br_self = 1;
    trk_note_on(&trk[k], nt, v);
    seq_out_on(&trk[k], nt, v);
    br_self = 0;
}
/* events_block, before the tracks' steps (the capture armed for a note on the boundary): adv clock units this block */
static void br_block(uint32_t adv)
{
    uint64_t pos = br_now(), endb = pos + adv;
    uint32_t k, i;
    if (!song.playing) {
        for (k = 0; k < NTRK; k++)
            if (br[k].phase)
                br_stop(k);
        return;
    }
    for (k = 0; k < NTRK; k++) {
        br_t *b = &br[k];
        const uint8_t *s = br_s[k];
        uint64_t iv = (uint64_t)BEAT_U * BR_IV32[s[BR_IVAL] & 7u] / 8u, of = (uint64_t)BEAT_U * (s[BR_OFS] & 15u) / 4u, nb;
        if (!((br_on >> k) & 1u)) {
            if (b->phase)
                br_stop(k);
            continue;
        }
        of %= iv;
        nb = pos <= of ? of : of + (pos - of + iv - 1u) / iv * iv;   /* the first INTERVAL boundary at or after pos */
        for (; nb < endb; nb += iv) {
            b->pulse_ms = fm1_ms;
            if (!b->phase && br_rand(100u) < s[BR_CHANCE]) {   /* CHANCE: the capture begins */
                b->phase = 1, b->n = 0, b->k = 0, b->shift = 0, b->vg = 32768;
                b->cs = nb;
                b->glen = br_glen(k);
                b->end = nb + (uint64_t)BEAT_U * (s[BR_GATE] ? s[BR_GATE] : 1u) / 4u;
            }
        }
        while (b->phase) {
            if (b->cs + b->glen > endb)                 /* (this repeat goes on into the next block) */
                break;
            if (b->phase == 1u) {                       /* the capture ends: the first repeat */
                b->phase = 2;
            } else {
                br_offs(k);
            }
            b->cs += b->glen;
            if (b->cs >= b->end || !b->n) {             /* GATE passed (or nothing to repeat): the last repeat's notes
                                                         * ended above */
                b->phase = 0, b->n = 0;
                break;
            }
            b->k++;
            b->glen = br_glen(k);
            b->shift = (uint8_t)(s[BR_PITCH] * b->k > 48u ? 48u : s[BR_PITCH] * b->k);
            b->vg = (uint16_t)((uint32_t)b->vg * (100u - (s[BR_DECAY] > 100u ? 100u : s[BR_DECAY])) / 100u);
            for (i = 0; i < b->n; i++) {                /* this repeat's notes that fall in this block */
                uint64_t t = b->cs + b->ev[i].off;
                if (b->ev[i].off < b->glen && t < endb)
                    br_play(k, i);
            }
        }
        if (b->phase == 2u)                             /* the notes later in a repeat begun in an earlier block */
            for (i = 0; i < b->n; i++) {
                uint64_t t = b->cs + b->ev[i].off;
                if (b->ev[i].off && b->ev[i].off < b->glen && t >= pos && t < endb && b->cs < pos)
                    br_play(k, i);
            }
    }
}
static void br_toggle(uint32_t k, int on)              /* top pads 5-8, FX CH notes 84-87 */
{
    k %= NTRK;
    if (on)
        br_on |= (uint8_t)(1u << k);
    else {
        br_on &= (uint8_t)~(1u << k);
        br_stop(k);
    }
}
#define BR_NOTE0 84u                                    /* FX CH: 84..87 BEAT REPEAT T1..T4 (on while held) */
