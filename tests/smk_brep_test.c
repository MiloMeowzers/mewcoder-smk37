/* SMK-37 BEAT REPEAT (firmware/src/smk_brep.h; smk37-devtools punch-page2-spec.md §3), on the host: a 1/16 pattern on
 * the drum track and on T1 at 120 BPM in 256-sample blocks, the repeater before the steps as events_block runs it.
 *   cc -o smk_brep_test tests/smk_brep_test.c && ./smk_brep_test */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define NTRK 4
#define TRK_DRUM 3
#define FS 44100
#define BEAT_U ((uint32_t)FS * 60u)
typedef struct { int dummy; } track_t;
static track_t trk[NTRK];
static uint32_t clk_beat, clk_pos, fm1_ms;
static struct { uint8_t playing; } song;

static int fails;
static void check(const char *what, int ok)
{
    printf("%-100s %s\n", what, ok ? "ok" : "FAIL");
    fails += !ok;
}

/* what was played: each note on, its track, note, velocity, the clock, whether the repeater played it */
#define LOGN 4096
static struct { uint32_t k, note, vel, rep; uint64_t at; } lg[LOGN];
static uint32_t nlg, noffs;
#include "../firmware/src/smk_brep.h"
static void trk_note_on(track_t *t, uint32_t note, uint32_t vel)
{
    if (br_on && !br_self && (vel = br_note(t, note, vel)) == 0u)
        return;
    if (nlg < LOGN)
        lg[nlg].k = (uint32_t)(t - trk), lg[nlg].note = note, lg[nlg].vel = vel, lg[nlg].rep = br_self,
        lg[nlg].at = br_now(), nlg++;
}
static void trk_note_off(track_t *t, uint32_t note) { (void)t, (void)note; noffs++; }
static void seq_out_on(const track_t *t, uint32_t note, uint32_t vel) { (void)t, (void)note, (void)vel; }
static void seq_out_off(const track_t *t, uint32_t note) { (void)t, (void)note; }

#define BPM 120u
#define BLK 256u
#define SIXT (BEAT_U / 4u)                              /* a 1/16 in clock units */
/* bars of play: each block the repeater, then the steps that start in it (a note a 1/16 on T1 and the drums) */
static void run(uint32_t bars)
{
    uint32_t adv = BLK * BPM, blocks = (uint32_t)((uint64_t)bars * 4u * BEAT_U / adv), b;
    uint64_t next = 0;
    nlg = 0, noffs = 0, clk_beat = 0, clk_pos = 0;
    song.playing = 1;
    br_reset();
    for (b = 0; b < blocks; b++) {
        uint64_t pos = br_now();
        br_block(adv);
        while (next < pos + adv) {                      /* this block's steps (at its start, as seq_tick fires them) */
            uint32_t step = (uint32_t)(next / SIXT) % 16u;
            trk_note_on(&trk[TRK_DRUM], 36u + step, 100u);
            trk_note_on(&trk[0], 60u + step, 100u);
            next += SIXT;
        }
        clk_pos += adv;
        while (clk_pos >= BEAT_U)
            clk_pos -= BEAT_U, clk_beat++;
        fm1_ms += 6u;
    }
    song.playing = 0;
    br_block(adv);
}
static uint32_t count(uint32_t k, int rep)
{
    uint32_t i, n = 0;
    for (i = 0; i < nlg; i++)
        n += lg[i].k == k && (int)lg[i].rep == rep;
    return n;
}

int main(void)
{
    char what[160];
    uint32_t i, n, ok;
    /* 1. the drums, defaults (1 bar, 1/16, GATE 4/16, WET 50): each bar the first 1/16 (note 36) is captured and played
     * again at 1/16, 2/16, 3/16 */
    br_on = 1u << TRK_DRUM;
    run(4);
    n = count(TRK_DRUM, 1);
    ok = n == 12u;
    for (i = 0; i < nlg; i++)
        if (lg[i].rep) {
            uint64_t into = lg[i].at % (4u * (uint64_t)BEAT_U);
            ok &= lg[i].note == 36u && into + BLK * BPM > SIXT && into < 4u * SIXT && lg[i].vel == 100u;   /* (a block early at most) */
        }
    snprintf(what, sizeof what, "the drums, 1 bar / 1/16 / GATE 4/16: 3 repeats a bar of the bar's first hit, full level (%u)", n);
    check(what, ok);
    check("the track's own hits go on beside the repeats (WET / DRY 50: 16 a bar)", count(TRK_DRUM, 0) == 64u);
    check("T1, not on: no repeats", count(0, 1) == 0u);
    /* 2. WET / DRY 100: only the repeats while they play */
    br_s[TRK_DRUM][BR_WD] = 100;
    run(2);
    snprintf(what, sizeof what, "WET / DRY 100: the track's own hits silent while the repeats play (%u of 32 heard)",
             count(TRK_DRUM, 0));
    check(what, count(TRK_DRUM, 0) == 32u - 6u && count(TRK_DRUM, 1) == 6u);
    br_s[TRK_DRUM][BR_WD] = 50;
    /* 3. MUTE: no repeat notes, the track as it is */
    br_mute = 1u << TRK_DRUM;
    run(2);
    check("MUTE: no repeat notes, every own hit", count(TRK_DRUM, 1) == 0u && count(TRK_DRUM, 0) == 32u);
    br_mute = 0;
    /* 4. CHANCE 0: never */
    br_s[TRK_DRUM][BR_CHANCE] = 0;
    run(2);
    check("CHANCE 0: never fires", count(TRK_DRUM, 1) == 0u);
    br_s[TRK_DRUM][BR_CHANCE] = 100;
    /* 5. DECAY 50, GRID 1/8 (index 2), GATE 8/16, INTERVAL 1/2 (index 4), OFFSET 4/16, on T1 with PITCH 2 */
    br_on = 1u;
    br_s[0][BR_DECAY] = 50, br_s[0][BR_GRID] = 2, br_s[0][BR_GATE] = 8, br_s[0][BR_IVAL] = 4, br_s[0][BR_OFS] = 4,
    br_s[0][BR_PITCH] = 2;
    run(2);
    n = count(0, 1);
    ok = n == 12u;                                      /* (GATE = INTERVAL: a boundary while it repeats is
                                                         * skipped, so it fires every other half bar: 2 x 2 notes x 3) */
    for (i = 0; i < nlg; i++)
        if (lg[i].rep && lg[i].k == 0u) {
            uint64_t hb = 2u * (uint64_t)BEAT_U, into = (lg[i].at + BLK * BPM + hb - 4u * SIXT) % hb;   /* from the
                                                         * boundary (OFFSET 4/16; a block early at most) */
            uint32_t r = (uint32_t)(into / (2u * SIXT)), want_v = 100u >> r;   /* (DECAY 50 % a repeat) */
            ok &= r >= 1u && r <= 3u && lg[i].vel == want_v &&
                  (lg[i].note == 64u - 2u * r || lg[i].note == 65u - 2u * r);   /* (the 1/8 at 4/16: steps 4, 5; PITCH 2) */
        }
    snprintf(what, sizeof what, "T1, 1/2 / 1/8 / GATE 8/16 / OFFSET 4/16 / DECAY 50 / PITCH 2: 3 repeats of 2 notes, halving (%u)", n);
    check(what, ok);
    /* 6. let go mid-repeat: it stops, the repeat's notes ended */
    {
        uint32_t offs0;
        run(1);
        offs0 = noffs;
        br_toggle(0, 0);
        check("switched off: no repeat playing", br[0].phase == 0u && noffs >= offs0);
    }
    printf(fails ? "SMK BREP TEST FAILED (%d)\n" : "smk_brep: all ok\n", fails);
    return fails != 0;
}
