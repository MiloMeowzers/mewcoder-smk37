/* SPDX-License-Identifier: GPL-3.0-only */
/* SMK-37, the vocoder build: the HARMONISER, Stage 1a (smk37-devtools harmoniser-spec.md §8; HARMONISER.md).
 * HOME > HARMONY ON: track 1 is the harmoniser, an Eventide H949 model, instead of a synth. Its "engine" (voice.c
 * track_render calls hm_render) runs in T1's unit, so it renders after core 1 has its job, beside it (§7i: the probe's
 * serial place before the hand-off held core 1 back). The track's chain runs on it (DIST, SLICER, LEVEL, PAN, its
 * reverb and chorus sends, STEM FX, its stem), less its FILTER and its DELAY SEND (fx.c: the H949 is the delay).
 * Each output sample (44.1 kHz):
 * - the input: IN L / IN R routed to HARMONY (usb.c uin_block, uin_dst[UD_HARM]) x VOICE (FADER BANK E, T1's column)
 *   plus the HARM sends of T2, T3 (not while it vocodes) and the drums (FADER BANK E), taken post-fader in the units
 *   (fx.c) a block before (hm_sb: the units of both cores write this block's, the harmoniser reads the last's);
 * - the memory (HM_LEN samples at 22.05 kHz, a pair's mean, as smk_tpunch.h PUNCH; SLICER STUT's part of smk_fxpool,
 *   STUT off meanwhile) written with the input + the feedback, soft-limited; REPEAT (NOTE REPEAT on T1) stops the
 *   writing, the slot still moving: the memory (~418 ms) plays round;
 * - three voices, each two taps half a segment apart read at the voice's ratio with a triangle crossfade (punch.c
 *   PX_OCT generalised: ALGORITHM 1, segment NORM 25 ms), interpolated, each faded in and out (~1.5 ms) with its note;
 * - the feedback FB MAIN (T1's FADER BANK D): the sounding voices' mean x FB (at most 0.95), into the memory through
 *   the soft limit, whose slope is never over 1: the loop's gain stays under 1 (FB LIMIT SAFE; the EQ flat in 1a);
 * - the output: the input (dry) and the voices' sum, half each (WET / DRY 50 %), on the track's scale.
 * The notes: T1's (keys, its sequencer pattern, MIDI in on its channel: voice.c trk_note_on) each set one voice to
 * its distance from C4 (C4 unison, C5 x2, C2 x0.25), up to 3, a fourth taking the oldest's voice; none: the voices
 * silent, the dry plays on. T1's FADER BANK B = PITCH (hm_pitch): the centre x1, down 24 semitones, up 12, all voices.
 * PITCH and FEEDBACK are the harmoniser's own (0 at power-on), not T1's FILTER and DELAY SEND: 67ba07e used those, so
 * a synth preset's delay send became feedback (a fifth fed back climbed octaves) and its filter a transposition.
 * With T1's SCALE on, each voice is the scale note at that distance above the SUNG note (rounded down into the scale)
 * and the tracker runs (only then): YIN on the dry input at 5.5 kHz (a frame ~20 ms), its note taken once two of the
 * last three frames agree, held through silence.
 * Stage 1b (§9, the HARMONY page, ui_hpage.c): the page's settings (hs): each voice's LEVEL, MICRO and function (NORM;
 * SHARP / FLAT: the voice follows its key as in NORM, MICRO its fine shift up to 1.07 / down to 0.93; 2026-11-13: they
 * sounded with no key at 1:1 and ignored the keys, and with VOICE ALL that locked all three: "the keys don't seem to
 * function anymore"), the control method (KEYS &
 * MIDI; MANUAL: the PITCH fader is the VOICE-selected voice's ratio, with no key; both: the keys, PITCH transposing,
 * the MANUAL voice sounding while no key holds it), INPUT LEVEL, IN / OUT (the input straight out, the memory still
 * written), the mutes (DRY, V1-V3), DRY LEVEL, the feedback's LOW / HIGH EQ (two shelves at 22.05 kHz,
 * smk_harm_eq.h) and FB LIMIT: SAFE (the loop's gain under 1 at the EQ's loudest frequency) or H949 (up to 1.25, the
 * soft limit holding the level: it can sustain itself). G1's interval pads play as T1's notes (hm_pad). Every
 * default is 1a's sound, bit for bit. */
#define SMK_HARM 1
#include "smk_harm_eq.h"             /* the feedback EQ's coefficients and INPUT LEVEL's gains (tools/gen_harm_eq.py) */
#define HM_LEN 7056u                 /* a lane's memory: 22.05 kHz samples, 320 ms (Stage 1c, harmoniser-spec.md §10a-2:
                                      * every lane the same, so REPEAT is the same everywhere; 1a / 1b: 9216, 418 ms) */
#define HM_SEG 551u                  /* the segment (NORM): 25 ms at 22.05 kHz */
#define HM_D0 4u                     /* the taps' least delay (storage samples) */
#define HM_TW 64u                    /* the tracker's window, 5.5 kHz samples (11.6 ms) */
#define HM_TLN 71u                   /* .. its lags 1 .. 71 (the search from HM_TLMIN: 689 .. 78 Hz) */
#define HM_TLMIN 8u
#define HM_TLB 5u                    /* .. lags a block (a frame: 15 blocks, ~20 ms) */
#define HM_TB (HM_TW + HM_TLN + 1u)  /* .. the snapshot */
#define HM_TRN 256u                  /* the tracker's dry ring (5.5 kHz, a power of two), in the pool after the memory */
#define HM_LENG (HM_LEN + 2u)        /* a lane's memory as stored: its HM_LEN slots, the guard slot (mem[HM_LEN], always
                                      * mem[0]: a tap's second read needs no wrap test, harmoniser-spec.md §13s) and a
                                      * spare int16 (alignment) */
#define HM_MEM (HM_LENG + HM_TRN + HM_TB)  /* (int16s of smk_fxpool; even) */
#define HM_FADE 512                  /* a voice's gain step a sample (Q15): ~64 samples in or out */
#define HM_GAIN(v) ((int32_t)(v) * 32768 / 100)   /* FADER BANK E's 0 .. 127 as a gain, Q15: 100 = 0 dB exactly, 127
                                                   * +2.1 dB */
_Static_assert((HM_MEM & 1u) == 0u, "the harmoniser's part of the pool stays 32-bit aligned after it");
_Static_assert(HM_TB < HM_TRN, "the snapshot fits the dry ring");
_Static_assert(2u * HM_TB + HM_SEG + HM_D0 < HM_LEN, "the reads stay in the memory");
/* the tracker's lags (Q8, 5.5 kHz) between MIDI note 38 + k and 39 + k (a quarter tone either side of each note) */
#define HM_NLO 38u
static const uint16_t HM_NOTE_LAG[41] = {18675, 17627, 16637, 15703, 14822, 13990, 13205, 12464, 11764, 11104, 10481,
                                         9893,  9337,  8813,  8319,  7852,  7411,  6995,  6603,  6232,  5882,  5552,
                                         5240,  4946,  4669,  4407,  4159,  3926,  3706,  3498,  3301,  3116,  2941,
                                         2776,  2620,  2473,  2334,  2203,  2080,  1963,  1853};

/* (hm_on, HOME > HARMONY: smk_vocoder.h, beside the routes that test it) */
static uint8_t hm_send[4] = {100, 0, 0, 0};   /* FADER BANK E: T1's column = VOICE (the input's level), then the HARM
                                       * sends of T2, T3, the drums; 0 .. 127 (100: 0 dB; kept: vocset) */
static uint8_t hm_int;               /* HOME > HARMONY > INTERVALS: 0 FIXED (the keys from C4, chromatic; the tracker at rest:
                                      * the default), 1 SCALE (from the sung note, in T1's ROOT / SCALE: the song's key);
                                      * kept, project.c vocset (the owner, 2026-11-12) */
static int16_t hm_pitch;             /* FADER BANK B, T1's fader: -64 .. 63 (0: x1; main.c smk_faders, its dead zone) */
static int16_t hm_fb;                /* FB MAIN 0 .. 127 */
static int16_t hm_wd = 64;           /* WET / DRY 0 .. 127 (64: half each, exactly) */
static int16_t hm_r22v;              /* LANE RATE, FADER BANK E's column 4 (harmoniser-spec.md §13s): 0 .. 127, 64 and up
                                      * 22K (every lane's voices once a pair); an A/B, not kept: 44.1 at power-on */
static volatile uint8_t hm_r22;      /* .. as the audio reads it (main.c smk_faders sets both) */
/* the LANE settings (HOME > LANE THIN, LANE RATE, HARMONY > LANE MONO; harmoniser-spec.md §13v; kept: project.c vocset).
 * Bit k: lane k (VOICE, T2, T3, DRUMS) */
#define HM_THIN_DEF 7u                /* LANE THIN: VOICE, T2, T3 may thin under load; the drums never (the owner) */
static uint8_t hm_thin = HM_THIN_DEF;
static uint8_t hm_lrate;             /* LANE RATE: 22K (the voices once a pair); bank E column 4 (hm_r22) on top */
static uint8_t hm_lmono = 1u;        /* LANE MONO: while the stems stream, the lanes mono (a track on the pair stereo) */
static volatile uint8_t hm_lmask;    /* .. this block's mono lanes (fx.c, before the units) */
static uint8_t hm_probe;             /* the probe (console harm probe, §13v-3): 1 every lane's feedback EQ off, 2 their
                                      * voices off; 0 as normal. Not kept */
/* the HARMONY page's settings (Stage 1b, harmoniser-spec.md §9; ui_hpage.c; kept: project.c vocset). The audio reads
 * them once a block */
enum { HF_NORM, HF_EXT, HF_SHARP, HF_FLAT, HF_N };   /* a voice's function (FUNCTION SELECT IN; EXTEND: Stage 2) */
#define HC_KEYS 1u                   /* the control method: KEYS & MIDI, MANUAL, both (3) */
#define HC_MAN 2u
typedef struct {
    int8_t mic[3];                   /* MICRO -100 .. 100: cents (NORM); SHARP / FLAT: its size, 0 .. 100 */
    uint8_t lv[3];                   /* LEVEL 0 .. 100 (linear %, 100: 0 dB, 1a's) */
    uint8_t fn[3];                   /* HF_* */
    uint8_t vsel;                    /* VOICE: 0 ALL, 1 .. 3 V1 .. V3 (the MANUAL voice: V1 with ALL) */
    uint8_t ctl;                     /* HC_KEYS, HC_MAN or both */
    uint8_t inl;                     /* INPUT LEVEL, HM_INL's step (HM_INL_0: 0 dB) */
    uint8_t low, high;               /* the feedback EQ's steps (HM_LOW_0, HM_HIGH_0: flat) */
    uint8_t fbl;                     /* FB LIMIT: 0 SAFE, 1 H949 */
    uint8_t mute;                    /* bit 0 DRY, bits 1 .. 3 V1 .. V3: off */
    uint8_t dry;                     /* DRY LEVEL 0 .. 100 (linear %) */
    uint8_t byp;                     /* IN / OUT: 1 OUT (the input straight through) */
    uint8_t fsel;                    /* FUNCTION SELECT: 0 IN (red, pitch), 1 OUT (green, delay: Stage 2) */
    int8_t pan[3];                   /* each voice's pan -64 .. 63 (0: centre; Stage 1c, §10b; the spare bytes of 1b: 0) */
} hset_t;
_Static_assert(sizeof(hset_t) == 22u, "hset_t: as kept in vocset");
static const hset_t HS_DEF = {{0, 0, 0}, {100, 100, 100}, {HF_NORM, HF_NORM, HF_NORM}, 0, HC_KEYS, HM_INL_0, HM_LOW_0,
                              HM_HIGH_0, 0, 0, 100, 0, 0, {0, 0, 0}};
static hset_t hs = {{0, 0, 0}, {100, 100, 100}, {HF_NORM, HF_NORM, HF_NORM}, 0, HC_KEYS, HM_INL_0, HM_LOW_0, HM_HIGH_0,
                    0, 0, 100, 0, 0, {0, 0, 0}};
static void hs_fix(hset_t *s)        /* every value in range (a load, the editor) */
{
    uint32_t v;
    for (v = 0; v < 3u; v++) {
        s->mic[v] = (int8_t)clamp(s->mic[v], -100, 100);
        s->lv[v] = (uint8_t)(s->lv[v] > 100u ? 100u : s->lv[v]);
        s->fn[v] = (uint8_t)(s->fn[v] == HF_SHARP || s->fn[v] == HF_FLAT ? s->fn[v] : HF_NORM);
    }
    s->vsel &= 3u;
    s->ctl &= 3u;
    if (!s->ctl)
        s->ctl = HC_KEYS;
    s->inl = (uint8_t)(s->inl % HM_INL_N), s->low = (uint8_t)(s->low % HM_LOW_N), s->high = (uint8_t)(s->high % HM_HIGH_N);
    s->fbl &= 1u, s->mute &= 15u, s->fsel &= 1u;
    s->byp = 0;                      /* IN / OUT: always IN after a load or power-on (the H949's performance switch, not a
                                      * setting: left on OUT by accident it passed the input dry, WET / DRY and all
                                      * else ignored, and stayed so; the owner 2026-11-13: "I can't silence the
                                      * unprocessed drums") */
    s->dry = (uint8_t)(s->dry > 100u ? 100u : s->dry);
    for (v = 0; v < 3u; v++)
        s->pan[v] = (int8_t)clamp(s->pan[v], -64, 63);
}
/* G1's 16 interval pads (the HK940's: top unison, 2nd, 3rd, 4th, 5th, 6th, 7th, octave up; bottom 2nd .. octave, two
 * octaves down), held or latched: the page sets them, hm_render plays them as T1's notes (60 + the interval) */
static volatile uint16_t hm_pad, hm_padre;   /* (hm_padre: latched pads whose voice was taken, played again: the page
                                              * sets it, hm_render takes it) */
static const int8_t HM_PADIV[16] = {0, 2, 4, 5, 7, 9, 10, 12, -2, -4, -5, -7, -9, -10, -12, -24};
/* (PITCH, FEEDBACK, VOICE IN, WET / DRY: FADER BANK E, HARM; the sends: F, SENDS; ui_fbank.c, main.c) */
static int16_t *hm_ring;             /* the VOICE lane's memory and the tracker's rings: smk_fxpool's start (fx.c sets it;
                                      * slicer.c owns it) */
#define hm_tring (hm_ring + HM_LENG)                 /* the dry input, 5.5 kHz */
#define hm_tb (hm_ring + HM_LENG + HM_TRN)           /* the frame's snapshot, scaled */
#define HM_T(t) (hm_on && (t) == &trk[0])
/* Stage 1c (harmoniser-spec.md §10): a LANE across each track sent to it, the harmoniser in its track's chain (fx.c
 * hm_insert: after the track's LEVEL, mute and punch-in, before its pan, sends and stem): VOICE (T1, the input), T2,
 * T3 (AUDIO: the vocoder is off while HARMONY is on), DRUMS. One set of controls (hb, worked out once a block on core
 * 0 before the units: hm_block); each lane its own memory, write slot, taps, fades, feedback and EQ state. Linear:
 * the lanes of the voice and of T2 are what one harmoniser of their sum gave (1b's sends); only the separation is new */
#ifndef HM_POOL
#define HM_POOL __attribute__((section(".pool")))   /* (.pool: zeroed at boot; the host tests define it empty) */
#endif
enum { HL_VOICE, HL_T2, HL_T3, HL_DRUM, HL_N };
typedef struct {
    int16_t *mem;                    /* HM_LEN storage samples */
    uint32_t w, t;                   /* the write slot; output samples (the pair's phase) */
    int32_t half, fb;                /* the pair's first sample; the feedback for the next write */
    int32_t ph[3], g[3];             /* each voice: its segment phase (Q16 storage samples), its fade (Q15) */
    int32_t eq[2][4];                /* the feedback EQ's state (LOW, HIGH) */
    uint8_t run;                     /* 0: cleared at its next block (just turned on: no stale audio) */
    uint8_t rok;                     /* the ramps' last values set (harmoniser-spec.md §13c) */
    int32_t rwg, rfb, rig;           /* WET / DRY, FEEDBACK, INPUT LEVEL at the last block's end: each ramped across the
                                      * block from there to this block's (a fader step is no gain step on the audio) */
    int32_t rkd, rsg;                /* the dry's share and bank F's send (the mixer's: fx.c hm_insert, mix_units) */
    uint16_t quiet;                  /* blocks its input, voices and feedback have all been silent (§13f) */
    int32_t lg;                      /* its input limiter's gain, Q15 (0: not started; harmoniser-spec.md §13q) */
    int32_t pvl, pvr;                /* its last output sample's sums (L, R; LANE RATE 22K's in-between sample, §13s) */
    uint8_t th[3];                   /* each voice thinned here under load (§13v-5: faded out; back when the load eases) */
    uint16_t vlv[3];                 /* .. each voice's peak here last block (the quietest goes first) */
    uint32_t blk;                    /* hm_block's count when it last ran (§13n: a lane not run lately holds no voice) */
    uint16_t vg[3];                  /* each voice: the assignment its release state is for (hv.gen) */
    uint8_t vq[3], vc[3];            /* .. released: blocks of its output under HM_QTHR; closed here (B) */
} hm_lane_t;
/* a lane's input limiter (§13q): a hot source (T2's pre-fader FM6) is turned down to HM_LIM instead of hard-clipped at
 * full scale; a block's peak is known before it plays, so the gain drops at once for the block (no clip) and comes back
 * over ~90 ms. Under HM_LIM the gain is 32768: the input exactly as before */
#define HM_LIM 29204                     /* -1 dBFS */
/* skip silent work (harmoniser-spec.md §13f): a lane whose input, voices and feedback have stayed under HM_QTHR (int16
 * scale: ~-80 dB) for longer than its memory (HM_LEN: 320 ms) does nothing until its input wakes it; REPEAT holds it
 * awake. Everything it would have made is under -80 dB, so the output is silence either way */
#define HM_QTHR 3
#define HM_QBLK 448u                     /* blocks of 32 (~325 ms) */
static hm_lane_t hm_ln[HL_N] HM_POOL;
static int16_t hm_lmem[HL_N - 1u][HM_LENG] HM_POOL;   /* T2, T3, DRUMS (42 KB; the VOICE lane's in smk_fxpool) */
static struct {                      /* the block's shared values (hm_block, core 0; the lanes read them) */
    int32_t gt[3], lg[3], pl[3], pr[3];   /* each voice: its gate (Q15), LEVEL x on (Q15), pan's left / right (Q12) */
    int32_t fbk, wg, dg, ig, kd;     /* feedback (Q15 over the voices), WET (Q15), DRY LEVEL (Q15), INPUT LEVEL (Q12), the
                                      * dry's gain in a lane's output (Q15: DRY LEVEL x (1 - WET); IN / OUT OUT: 32768) */
    int32_t cq[2][5];                /* the feedback EQ's coefficients */
    uint8_t eql, eqh, byp;
    uint8_t rl[3];                   /* each voice released, its tail open while each lane's output of it sounds (§13n B) */
    uint16_t gen[3];                 /* .. its assignment (hv.gen) */
} hb HM_POOL;
static uint8_t hm_spread;            /* G3 K5 SPREAD 0 .. 100 (V1 left, V3 right; kept: vocset) */
static uint8_t hm_vmode, hm_vpark;   /* VOCODER > MODE set aside while HARMONY is on (the vocoder off, MODE AUDIO: §10a-2);
                                      * hm_vpark: a load gave it before HARMONY was taken (project.c vocv_put) */
#define HM_LANE_ON(k) (hm_on && ((k) == HL_VOICE || hm_send[(k) & 3u]))   /* (a track's lane: bank F above 0) */
static struct {
    uint8_t note[3];                 /* each voice's note (0: none) */
    uint32_t age[3], seq;            /* .. its order (the oldest the lowest); the next */
    volatile uint8_t rep;            /* REPEAT: no lane's memory written */
    uint8_t run;                     /* running (the lanes cleared at the start) */
    int32_t dinc[3];                 /* each voice: the delay's change an output sample ((1 - ratio) x 32768, Q16) */
    uint32_t tw;                     /* the dry ring's write count */
    int32_t dacc;                    /* .. the 8-sample sum */
    uint32_t lag, sum;               /* the next lag (0: take a snapshot); the running sum of d */
    uint16_t cm[HM_TLN + 1u];        /* the normalised d, Q12 (4096 = 1), each lag */
    uint8_t p1, p2;                  /* the last two frames' notes (0: unvoiced) */
    volatile uint8_t tnote;          /* the sung note (0: none yet) */
    volatile uint32_t per;           /* the last frame's period, 5.5 kHz samples Q8 (0: no pitch) */
    volatile uint32_t frames, voiced;
    volatile int32_t pk_in, pk_out;  /* the peaks since the console read them */
    uint8_t on[3];                   /* each voice sounding this block (a note, MANUAL, SHARP / FLAT) */
    uint16_t padl;                   /* the pads as last played */
    volatile int32_t up[6];          /* the page's peaks (it reads and clears them): the input, V1 .. V3, the output, the
                                      * feedback into the memory before its soft limit */
    volatile int32_t lpi[4], lpo[4], lpf[4];   /* (§12) each lane's input (after INPUT LEVEL), wet output and memory
                                      * write before its soft limit (feedback in) peaks, int16 scale, since the console
                                      * read them ("harm": the T2 / drums levels; 32767 = clipped) */
} hm;
#define HM_UP_IN 0u
#define HM_UP_V 1u
#define HM_UP_OUT 4u
#define HM_UP_FB 5u
static uint32_t hm_mvoice(void) { return hs.vsel ? (hs.vsel - 1u) % 3u : 0u; }   /* the MANUAL voice */
static uint32_t scale_mask(const track_t *t);        /* (seq.c) */
/* NOTE REPEAT = REPEAT (HARMONY, T1 selected; ui_input.c): its press (ms), held; as the LED and the header show it:
 * steady while held, flashing while latched (the owner, 2026-11-12) */
static uint32_t hm_rep_t0;
static uint8_t hm_rep_held, hm_rep_pad;              /* (hm_rep_pad: the HARMONY page's REPEAT pad held, ui_hpage.c) */
static int hm_rep_shown(void) { return hm.rep && (hm_rep_held || hm_rep_pad || ((fm1_ms / 250u) & 1u)); }

/* the notes (voice.c trk_note_on / off / all_off, on T1 while HARMONY is on; the pads, hm_block). Who holds a voice
 * (harmoniser-spec.md §13n): a key (or MIDI in), the sequence (with the arp and ROLL) or a pad, as bits, so the same
 * note held by two of them stays until both let go */
#define HM_SK 1u                     /* a key, MIDI in */
#define HM_SQ 2u                     /* the sequence, the arp, ROLL (seq.c sets hm_src around them) */
#define HM_SP 4u                     /* an interval pad */
#define HM_SWB 3u                    /* a steal's fade out, blocks (HM_FADE: 64 samples, two blocks; one more) */
#define HM_RQB 8u                    /* a released voice closes in a lane after this many blocks of its output under
                                      * HM_QTHR (5.8 ms) */
#define HM_RCAP 2756u                /* .. and everywhere after 2 s at the most (a drone on a send lane: a released
                                      * interval does not run on) */
#define HM_WN 4u                     /* notes that can wait for a voice */
static uint8_t hm_src;               /* the holder of a T1 note now (voice.c trk_note_on / off): 0 = HM_SK; seq.c sets
                                      * HM_SQ around the sequencer, the arps, ROLL; hm_pad_note HM_SP */
static struct {
    uint8_t hold[3];                 /* each voice's holders (0 with a note: released, its tail sounding, §13n B) */
    uint8_t pn[3], ps[3], sw[3];     /* a voice being taken (§13n D): the next note, its holders, blocks of fade left */
    uint16_t gen[3];                 /* each voice's assignment count (the lanes' release state follows it) */
    uint16_t rb[3];                  /* blocks since its release */
    uint8_t wn[HM_WN], ws[HM_WN], wc;   /* notes waiting for a voice (§13n E), their holders, how many */
    uint32_t nblk;                   /* hm_block's count (a lane that ran lately: hm_lane_t blk) */
    volatile uint32_t thins, unthins;   /* (§13v-5, the console's, cleared: lane voices thinned / back) */
    volatile uint32_t steals, waits, drops;   /* the console's (harm, cleared): a held voice taken (faded), a note that
                                      * had to wait, one let go before it got a voice */
} hv;
static uint32_t hm_free(uint32_t v) { return !hm.note[v] && !hv.sw[v]; }
/* the voice a note may take (§13n E): a free one, else the released one longest in its tail, else the oldest held by
 * the sequence alone; a voice a key or a pad holds is never taken (3: none). res: MANUAL's voice */
static uint32_t hm_vpick(uint32_t res)
{
    uint32_t v, k = 3u;
    for (v = 0; v < 3u; v++)
        if (v != res && hm_free(v))
            return v;
    for (v = 0; v < 3u; v++)
        if (v != res && hm.note[v] && !hv.hold[v] && !hv.sw[v] && (k == 3u || hv.rb[v] > hv.rb[k]))
            k = v;
    if (k < 3u)
        return k;
    for (v = 0; v < 3u; v++)
        if (v != res && !hv.sw[v] && hv.hold[v] == HM_SQ && (k == 3u || hm.age[v] < hm.age[k]))
            k = v;
    return k;
}
/* voice k to note: free, at once; sounding (held or in its tail), faded out first at its old interval, then the new
 * one faded in (§13n D) */
static void hm_take(uint32_t k, uint32_t note, uint32_t src)
{
    if (hm.note[k]) {
        if (hv.hold[k])
            hv.steals++;
        hv.pn[k] = (uint8_t)note, hv.ps[k] = (uint8_t)src, hv.sw[k] = HM_SWB;
        return;
    }
    hm.note[k] = (uint8_t)note, hv.hold[k] = (uint8_t)src, hm.age[k] = ++hm.seq;
    hv.gen[k]++, hv.rb[k] = 0;
}
static uint32_t hm_res(void) { return (hs.ctl & 3u) == HC_MAN ? hm_mvoice() : 3u; }   /* (MANUAL alone: its voice is
                                                       * the fader's) */
static void hm_note_on(uint32_t note, uint32_t src)
{
    uint32_t v, k;
    note = note ? note & 127u : 1u;
    src = src ? src : HM_SK;
    for (v = 0; v < 3u; v++) {
        if (hv.sw[v] && hv.pn[v] == note) {
            hv.ps[v] |= (uint8_t)src;                   /* (taking a voice already) */
            return;
        }
        if (hm.note[v] == note && !hv.sw[v]) {
            hv.hold[v] |= (uint8_t)src;                 /* (held already, or back from its tail) */
            return;
        }
    }
    for (v = 0; v < hv.wc; v++)
        if (hv.wn[v] == note) {
            hv.ws[v] |= (uint8_t)src;
            return;
        }
    k = hm_vpick(hm_res());
    if (k < 3u) {
        hm_take(k, note, src);
    } else if (hv.wc < HM_WN) {                         /* no voice may be taken: it waits while held */
        hv.wn[hv.wc] = (uint8_t)note, hv.ws[hv.wc] = (uint8_t)src, hv.wc++;
        hv.waits++;
    } else {
        hv.waits++, hv.drops++;
    }
}
static void hm_note_off(uint32_t note, uint32_t src)
{
    uint32_t v, j;
    note = note ? note & 127u : 1u;
    src = src ? src : HM_SK;
    for (v = 0; v < 3u; v++) {
        if (hv.sw[v] && hv.pn[v] == note) {
            hv.ps[v] &= (uint8_t)~src;
            if (!hv.ps[v])
                hv.pn[v] = 0;                           /* (let go before it got the voice: the voice frees) */
        }
        if (hm.note[v] == note && !hv.sw[v] && hv.hold[v]) {
            hv.hold[v] &= (uint8_t)~src;
            if (!hv.hold[v])
                hv.rb[v] = 0;                           /* released: its tail (§13n B) */
        }
    }
    for (v = 0; v < hv.wc; v++)
        if (hv.wn[v] == note) {
            hv.ws[v] &= (uint8_t)~src;
            if (!hv.ws[v]) {
                hv.drops++;
                for (j = v; j + 1u < hv.wc; j++)
                    hv.wn[j] = hv.wn[j + 1u], hv.ws[j] = hv.ws[j + 1u];
                hv.wc--;
            }
            break;
        }
}
static void hm_all_off(void)
{
    hm.note[0] = hm.note[1] = hm.note[2] = 0;
    memset(hv.hold, 0, sizeof hv.hold), memset(hv.pn, 0, sizeof hv.pn), memset(hv.sw, 0, sizeof hv.sw);
    hv.wc = 0;
}

/* the memory at a delay of d (Q16 storage samples) behind the newest slot, interpolated */
static inline int32_t hm_read(const int16_t *hm_ring, int32_t wq, int32_t d)   /* (hm_ring: the lane's memory) */
{
    int32_t p = wq - d, a, b;
    uint32_t i;
    if (p < 0)
        p += (int32_t)(HM_LEN << 16);
    i = (uint32_t)p >> 16;
    a = hm_ring[i];
    b = hm_ring[i + 1u];                                /* (slot HM_LEN: the guard, slot 0's copy, §13s) */
    return a + (((b - a) * (int32_t)(((uint32_t)p >> 1) & 0x7FFFu)) >> 15);
}

/* the tracker (the probe's, at div 8, tlb 5: harmoniser-spec.md §7i). The frame: the newest HM_TB samples of the dry
 * ring, scaled so the squared differences fit 32 bits (the peak under 1024) */
static void hm_snap(void)
{
    uint32_t j, s = 0, i = hm.tw - HM_TB;
    int32_t pk = 0;
    for (j = 0; j < HM_TB; j++) {
        int32_t x = hm_tring[(i + j) & (HM_TRN - 1u)], ax = x < 0 ? -x : x;
        hm_tb[j] = (int16_t)x;
        pk = ax > pk ? ax : pk;
    }
    while ((pk >> s) >= 1024)
        s++;
    for (j = 0; j < HM_TB; j++)
        hm_tb[j] = (int16_t)(hm_tb[j] >> s);
    hm.sum = 0;
}
/* the frame's end: its period from the normalised curve (Q8 lags, 0: none) */
static uint32_t hm_pick(void)
{
    uint32_t t, best = 0, h, k;
    const uint16_t *cm = hm.cm;
    for (t = HM_TLMIN; t < HM_TLN; t++)                 /* the first dip under 0.15, to its bottom */
        if (cm[t] < 614u) {
            while (t + 1u < HM_TLN && cm[t + 1u] < cm[t])
                t++;
            best = t;
            break;
        }
    if (!best) {                                        /* none: the deepest, if under 0.35 */
        uint32_t m = 1434u;
        for (t = HM_TLMIN; t < HM_TLN; t++)
            if (cm[t] < m)
                m = cm[t], best = t;
        if (!best)
            return 0;
    }
    if (best + 1u >= HM_TLN)                            /* at the lags' end: below the range, not a pitch */
        return 0;
    for (k = 3u; k >= 2u; k--) {                        /* the octave check: a dip near lag / 3, lag / 2 nearly as deep */
        h = (best + k / 2u) / k;
        if (h > HM_TLMIN && h + 1u < HM_TLN) {
            uint32_t c = h;
            c = cm[h - 1u] < cm[c] ? h - 1u : c;
            c = cm[h + 1u] < cm[c] ? h + 1u : c;
            if (cm[c] < 614u && cm[c] <= cm[best] + 410u && cm[c] <= cm[c - 1u] && cm[c] <= cm[c + 1u]) {
                best = c;
                break;
            }
        }
    }
    {   /* a parabola through the dip */
        int32_t a = cm[best - 1u], b = cm[best], c = cm[best + 1u], den = a - 2 * b + c, fr = 0;
        if (den > 0)
            fr = ((a - c) * 128) / den;
        return (best << 8) + (uint32_t)clamp(fr, -128, 128);
    }
}
/* a period's note (MIDI), or 0 */
static uint32_t hm_per_note(uint32_t per)
{
    uint32_t k = 0;
    if (!per)
        return 0;
    while (k < 41u && HM_NOTE_LAG[k] > per)
        k++;
    return HM_NLO + k;
}
/* HM_TLB lags of the frame; at its end, the period and the note (two of the last three frames agreeing) */
static void hm_lags(void)
{
    uint32_t k, j;
    for (k = 0; k < HM_TLB; k++) {
        uint32_t tau = hm.lag, d = 0;
        if (tau == 0u) {
            hm_snap();
            hm.lag = 1;
            continue;
        }
        for (j = 0; j < HM_TW; j++) {
            int32_t e = hm_tb[j] - hm_tb[j + tau];
            d += (uint32_t)(e * e);
        }
        d >>= 8;                                        /* (<= 1.1M: d x lag and the sum fit 32 bits) */
        hm.sum += d;
        {   /* the normalised d = d x lag / sum, Q12 (one hardware divide) */
            uint32_t sh = hm.sum >> 12, c = sh ? d * tau / sh : 65535u;
            hm.cm[tau] = (uint16_t)(c > 65535u ? 65535u : c);
        }
        if (++hm.lag > HM_TLN) {                        /* the frame's end */
            uint32_t nt;
            hm.per = hm_pick();
            nt = hm_per_note(hm.per);
            hm.frames++;
            hm.voiced += nt != 0u;
            if (nt && (nt == hm.p1 || nt == hm.p2))
                hm.tnote = (uint8_t)nt;                 /* (unvoiced, or no two agreeing: the last note held) */
            hm.p2 = hm.p1, hm.p1 = (uint8_t)nt;
            hm.lag = 0;
        }
    }
}

/* a voice's (1 - ratio) x 32768 (Q16 storage samples an output sample): s16 = its distance in 1/16 semitones */
static int32_t hm_dinc(int32_t s16)
{
    uint32_t b = pitch_inc(960u), r;                    /* (C4: the table holds two octaves down, one up from it) */
    s16 = clamp(s16, -384, 192);
    r = (uint32_t)(((uint64_t)pitch_inc((uint32_t)(960 + s16)) << 16) / b);   /* the ratio, Q16 */
    return 32768 - (int32_t)(r >> 1);
}
/* .. with ct cents more (MICRO; 0: hm_dinc's, exactly), the table read between its 1/16 semitone steps */
static int32_t hm_dincf(int32_t s16, int32_t ct)
{
    int32_t q, i, fr;
    uint32_t b, a0, a1, r;
    if (!ct)
        return hm_dinc(s16);
    q = clamp(s16 * 100 + ct * 16, -38400, 19200);     /* (1/1600 semitones) */
    i = q >= 0 ? q / 100 : -((-q + 99) / 100);
    fr = q - i * 100;
    b = pitch_inc(960u);
    a0 = pitch_inc((uint32_t)(960 + i));
    a1 = i < 192 ? pitch_inc((uint32_t)(961 + i)) : a0;
    r = (uint32_t)((((uint64_t)a0 * (uint32_t)(100 - fr) + (uint64_t)a1 * (uint32_t)fr) << 16) / (100u * (uint64_t)b));
    return 32768 - (int32_t)(r >> 1);
}
/* the voices this block: which sound, and their ratios (T1's notes, SCALE, PITCH, the page's control method, MICRO,
 * SHARP / FLAT) */
static void hm_ratios(const track_t *t)
{
    uint32_t v, sc = hm_int != 0, tn = hm.tnote, m = hm_mvoice(), ctl = hs.ctl & 3u;
    int32_t f = hm_pitch, pit = f < 0 ? f * 6 : f * 192 / 63;   /* PITCH: -64 .. 63 -> -24 .. +12 semitones */
    if (!ctl)
        ctl = HC_KEYS;
    for (v = 0; v < 3u; v++) {
        int32_t n = hm.note[v], s, ct = hs.mic[v], fn = hs.fn[v];
        int man = (ctl & HC_MAN) && v == m && (ctl == HC_MAN || !n);   /* (both: the MANUAL voice while no key holds
                                                         * it) */
        hm.on[v] = (uint8_t)(n || man);
        if (!hm.on[v])
            continue;                                   /* (fading out: the ratio it had) */
        if (fn == HF_SHARP || fn == HF_FLAT) {          /* MICRO as the H949's fine shift: 1:1 up to 1.07 / down to
                                                         * 0.93, finer near 1:1 (to ~1.001), on the voice's interval */
            int32_t a = ct < 0 ? -ct : ct;
            ct = fn == HF_SHARP ? a * a * 117 / 10000 : -(a * a * 126 / 10000);
        }
        if (man) {                                      /* MANUAL: the fader is the ratio */
            hm.dinc[v] = hm_dincf(pit, ct);
            continue;
        }
        s = n - 60;
        if (sc && tn) {                                 /* SCALE: the scale note s above the sung note, rounded down (a
                                                         * CHR scale: s itself) */
            uint32_t mask = scale_mask(t), root = (uint32_t)t->p[P_ROOT] % 12u;
            int32_t m = (int32_t)tn + s, guard = 12;
            while (guard-- && !((mask >> (uint32_t)((m - (int32_t)root + 120) % 12)) & 1u))
                m--;
            s = m - (int32_t)tn;
        }
        hm.dinc[v] = hm_dincf(s * 16 + ((ctl & HC_KEYS) ? pit : 0), ct);   /* (MANUAL alone: PITCH is its voice's) */
    }
}
/* the readout: voice v's ratio x 1000 (1000: 1:1) */
static uint32_t hm_ratio1000(uint32_t v) { return (uint32_t)((((int64_t)(32768 - hm.dinc[v % 3u]) * 2000) + 32768) >> 16); }
/* a feedback EQ section (direct form 1, Q HM_EQ_Q), its state 8 bits finer than the samples (a 400 Hz shelf's poles
 * sit near 1 at 22.05 kHz: with the state in whole samples its rounding came back ~30 times over, -13 dB for -12 at
 * low levels), the output rounded toward 0 as the loop is */
static inline int32_t hm_bq(int32_t *z, const int32_t *c, int32_t x)
{
    int32_t x8 = x * 256, y8;
    int64_t a = (int64_t)c[0] * x8 + (int64_t)c[1] * z[0] + (int64_t)c[2] * z[1] - (int64_t)c[3] * z[2] -
                (int64_t)c[4] * z[3];
    a >>= HM_EQ_Q;
    y8 = a > 33554431 ? 33554431 : a < -33554431 ? -33554431 : (int32_t)a;   /* (+-131072 samples) */
    z[1] = z[0], z[0] = x8, z[3] = z[2], z[2] = y8;
    return (y8 + (y8 < 0 ? 255 : 0)) >> 8;
}

/* a lane at rest: no stale memory, its voices' phases spread, their fades at 0 */
static void hm_lane_start(uint32_t k)
{
    hm_lane_t *L = &hm_ln[k % HL_N];
    memset(L, 0, sizeof *L);
    L->mem = k ? hm_lmem[(k - 1u) % (HL_N - 1u)] : hm_ring;
    memset(L->mem, 0, HM_LENG * sizeof L->mem[0]);
    L->ph[1] = (int32_t)(HM_SEG << 14), L->ph[2] = (int32_t)(HM_SEG << 15);
    L->run = 1;
}
/* the harmoniser's start: every lane at rest, the tracker's rings clear (the held notes kept) */
static void hm_start(void)
{
    uint8_t nt[3] = {hm.note[0], hm.note[1], hm.note[2]};
    uint32_t ag[3] = {hm.age[0], hm.age[1], hm.age[2]}, sq = hm.seq, k;
    memset(hm_ring, 0, HM_MEM * sizeof hm_ring[0]);
    memset(&hm, 0, sizeof hm);
    memcpy(hm.note, nt, sizeof nt), memcpy(hm.age, ag, sizeof ag), hm.seq = sq;
    for (k = 0; k < HL_N; k++)
        hm_lane_start(k);
    hm.run = 1;
}
/* a gentle ceiling on a lane's wet (int16 scale): untouched up to K (-2.5 dBFS), then bent towards full scale (the
 * three voices and the feedback reached 1.3 x full scale, 2026-11-13: the master's limiter worked on it, a click in
 * the headphones only) */
static inline int32_t hm_knee(int32_t y)
{
    const int32_t K = 24576, M = 32767 - 24576;
    int32_t a = y < 0 ? -y : y;
    if (a <= K)
        return y;
    a -= K;                                             /* (a * M fits 32 bits up to 262143: a 32-bit divide, the
                                                         * same result; past it, as before) */
    a = K + (a <= 262143 ? a * M / (a + M) : (int32_t)(((int64_t)a * M) / (a + M)));
    return y < 0 ? -a : a;
}
static void hm_pad_note(uint32_t note, uint32_t on);   /* (seq.c: a pad as T1's keys play a note, §13m) */
/* once a block on core 0, before the units (fx.c): the pads as notes, the voices' ratios and gates, the shared gains */
static void __attribute__((noinline)) hm_block(void)
{
    uint32_t v, nn = 0, li = hs.low % HM_LOW_N, hi = hs.high % HM_HIGH_N;
    int32_t fbk, wg, dg;
    if (!hm.run)
        hm_start();
    {   /* G1's pads: as notes (here, before the units, as the sequencer's): T1's notes as its keys' (MIDI out, recorded:
         * §13m, seq.c hm_pad_note); a latched pad played again while it waits for a voice: asked again, no MIDI */
        uint32_t pd = hm_pad, mv = pd ^ hm.padl, re = hm_padre & pd & ~mv, k;
        hm_padre = 0;
        for (k = 0; (mv | re) && k < 16u; k++) {
            uint32_t nt = (uint32_t)(60 + HM_PADIV[k]);
            if ((mv >> k) & 1u)
                hm_pad_note(nt, (pd >> k) & 1u);
            else if ((re >> k) & 1u)
                hm_note_on(nt, HM_SP);
        }
        hm.padl = (uint16_t)pd;
    }
    {   /* §13n: the voices' turns this block. A voice being taken: its fade out done, the new note (or free, if that was
         * let go meanwhile); a released voice: closed once every lane that runs has closed it (its output quiet), or
         * after HM_RCAP; then the notes waiting take what is free (the pads' first) */
        uint32_t k, j, pass;
        hv.nblk++;
        for (v = 0; v < 3u; v++) {
            if (hv.sw[v]) {
                if (--hv.sw[v])
                    continue;
                hm.note[v] = 0;
                if (hv.pn[v]) {
                    hm.note[v] = hv.pn[v], hv.hold[v] = hv.ps[v], hm.age[v] = ++hm.seq;
                    hv.gen[v]++, hv.rb[v] = 0;
                }
                hv.pn[v] = hv.ps[v] = 0;
                continue;
            }
            if (hm.note[v] && !hv.hold[v]) {
                uint32_t open = 0;
                if (hv.rb[v] < 0xFFFFu)
                    hv.rb[v]++;
                for (k = 0; k < HL_N; k++) {
                    const hm_lane_t *L = &hm_ln[k];
                    if (L->run && L->blk && hv.nblk - L->blk <= 2u && L->quiet < HM_QBLK && (L->vg[v] != hv.gen[v] || !L->vc[v]))
                        open = 1;
                }
                if (!open || hv.rb[v] >= HM_RCAP)
                    hm.note[v] = 0;
            }
        }
        for (pass = 0; pass < 2u && hv.wc; pass++)     /* (pass 0: the pads' notes; 1: the rest) */
            for (j = 0; j < hv.wc;) {
                if (!pass && !(hv.ws[j] & HM_SP)) {
                    j++;
                    continue;
                }
                k = hm_vpick(hm_res());
                if (k == 3u)
                    break;
                hm_take(k, hv.wn[j], hv.ws[j]);
                for (v = j; v + 1u < hv.wc; v++)
                    hv.wn[v] = hv.wn[v + 1u], hv.ws[v] = hv.ws[v + 1u];
                hv.wc--;
            }
    }
    hm_ratios(&trk[0]);
    for (v = 0; v < 3u; v++) {
        int32_t p = hs.pan[v] + (v == 0u ? -(int32_t)hm_spread * 64 / 100 : v == 2u ? (int32_t)hm_spread * 63 / 100 : 0);
        p = clamp(p, -64, 63);                          /* (SPREAD: V1 left, V3 right, V2 as it is) */
        hb.gt[v] = hm.on[v] && !hv.sw[v] ? 32767 : 0;   /* (§13n D: a voice being taken fades out) */
        hb.rl[v] = (uint8_t)(hm.note[v] && !hv.hold[v] && !hv.sw[v] && hm.on[v]);   /* (B: released, each lane its own
                                                         * close) */
        hb.gen[v] = hv.gen[v];
        nn += hm.on[v] != 0;
        hb.lg[v] = (hs.mute >> (v + 1u)) & 1u ? 0 : (int32_t)(hs.lv[v] > 100u ? 100u : hs.lv[v]) * 32768 / 100;
        hb.pl[v] = 4096 - (p > 0 ? p * 64 : 0), hb.pr[v] = 4096 + (p < 0 ? p * 64 : 0);   /* (the tracks' PAN law) */
    }
    if (hs.fbl & 1u) {                                  /* FB MAIN: H949, to 1.25 over the sounding voices */
        fbk = clamp(hm_fb, 0, 127) * 323 / (nn ? nn : 1);
    } else {                                            /* SAFE: to 0.95, under the EQ's loudest gain */
        fbk = clamp(hm_fb, 0, 127) * 246 / (nn ? nn : 1);
        if (li != HM_LOW_0)
            fbk = fbk * 4096 / HM_LOW_PK[li];
        if (hi != HM_HIGH_0)
            fbk = fbk * 4096 / HM_HIGH_PK[hi];
    }
    memcpy(hb.cq[0], HM_LOW[li], sizeof hb.cq[0]), memcpy(hb.cq[1], HM_HIGH[hi], sizeof hb.cq[1]);
    wg = hm_wd >= 127 ? 32768 : clamp(hm_wd, 0, 127) * 256;   /* WET / DRY, Q15: 64 = 16384 */
    dg = hs.mute & 1u ? 0 : (int32_t)(hs.dry > 100u ? 100u : hs.dry) * 32768 / 100;   /* DRY LEVEL */
    hb.fbk = fbk, hb.wg = wg, hb.dg = dg, hb.ig = HM_INL[hs.inl % HM_INL_N];
    hb.byp = (uint8_t)(hs.byp & 1u), hb.eql = (uint8_t)(li != HM_LOW_0), hb.eqh = (uint8_t)(hi != HM_HIGH_0);
    hb.kd = hb.byp ? 32768 : dg;         /* (§13p: DRY LEVEL / the DRY pad, every lane's dry trim; the balance is bank F's,
                                          * fx.c hm_wet / hm_kd) */
    for (v = 0; v < HL_N; v++)                          /* (a track's lane turned off: cleared when it comes back) */
        if (v && !HM_LANE_ON(v))
            hm_ln[v].run = 0;
}
/* lane k: n inputs (int16 scale, the lane's input level applied) into its memory; its voices' wet, WET applied, into
 * wl / wr (each voice at its pan) and, when wv, each voice apart (the drums' channels, §10c-2). The dry is the caller's
 * (hb.kd: its own signal, mono or stereo). The VOICE lane runs the tracker (INTERVALS SCALE) */
/* hm_lane in RAM (harmoniser-spec.md §13v-9): the vocoder build's .ram_hot (fx.c RAMHOT's attributes; copied at boot,
 * out of the flash and the cache both cores share). It may call nothing in the flash: its callers start the lane
 * (hm_lane_start) and reach it through TAIL_FAR */
#if BOARD_SMK37 && SMK_UAC_IN_PROBE && SMK_VOC_LEAN
#define HM_RAMHOT __attribute__((section(".ram_hot"), noinline, used, flatten))
#else
#define HM_RAMHOT __attribute__((noinline))
#endif
static uint32_t HM_RAMHOT hm_lane(uint32_t k, const int32_t *in, int32_t *wl, int32_t *wr,
                                                  int32_t (*wv)[VOC_MAXN], uint32_t n)   /* 0: skipped, silent (§13f) */
{
    hm_lane_t *L = &hm_ln[k % HL_N];
    const int16_t *mr;
    int16_t *mw;
    uint32_t i, v, sc = k == HL_VOICE && hm_int != 0;
    int32_t pk = hm.pk_in, po = hm.pk_out, ui_in = 0, ui_out = 0, ui_fb = 0, ui_v[3] = {0, 0, 0};
    int32_t fbk = hb.fbk, wg = 32768, ig = hb.ig, rep = hm.rep, byp = hb.byp, fb0, ig0;   /* (§13o: WET /
                                                         * DRY is T1's; a send lane's balance is bank F's, fx.c; the
                                                         * lane's own WET is unity: §13s, no product for it) */
    TIM_VOC_T(tl);                                      /* (§12: each lane's time, console "timing" ln_*) */
    L->blk = hv.nblk;                                   /* (started by its caller: hm_lane_start is in the flash) */
    for (v = 0; v < 3u; v++)                            /* (§13n B: a new assignment, or held again: open) */
        if (L->vg[v] != hb.gen[v] || !hb.rl[v])
            L->vg[v] = hb.gen[v], L->vq[v] = 0, L->vc[v] = 0;
    if (L->quiet >= HM_QBLK && !rep) {                  /* (§13f: asleep; a block of input above the floor wakes it) */
        uint32_t m = n > VOC_MAXN ? VOC_MAXN : n, a = 0;
        for (i = 0; i < m; i++) {
            int32_t x = (in[i] * ig) >> 12;
            a |= (uint32_t)(x > HM_QTHR || x < -HM_QTHR);
        }
        if (!a) {
            for (i = 0; i < m; i++)
                wl[i] = wr[i] = 0;
            if (wv)
                for (v = 0; v < 3u; v++)
                    for (i = 0; i < m; i++)
                        wv[v][i] = 0;
            L->rwg = wg, L->rfb = fbk, L->rig = ig;     /* (the ramps start where the controls are when it wakes) */
            for (v = 0; v < 3u; v++)
                L->vc[v] = hb.rl[v];                    /* (silent: a released voice closed here) */
            TIM_ADD(TS_LV + k % HL_N, TIM_NOW() - tl);
            return 0u;
        }
        L->quiet = 0;
    }
    if (!L->rok)                                        /* (§13c: the ramps from the last block's values) */
        L->rwg = wg, L->rfb = fbk, L->rig = ig, L->rok = 1;
    fb0 = L->rfb, ig0 = L->rig;
    L->rwg = wg, L->rfb = fbk, L->rig = ig;
    {
        const int32_t fb1 = fbk, ig1 = ig;
        int32_t lg0 = L->lg ? L->lg : 32768, lg1;
        {   /* (§13q: the block's peak after INPUT LEVEL, the limiter's gain for it) */
            int32_t im = ig0 > ig1 ? ig0 : ig1, pkb = 0, t;
            uint32_t m = n > VOC_MAXN ? VOC_MAXN : n;
            for (i = 0; i < m; i++) {
                int32_t a = (in[i] * im) >> 12;
                a = a < 0 ? -a : a;
                pkb = a > pkb ? a : pkb;
            }
            t = pkb > HM_LIM ? (int32_t)(((int64_t)HM_LIM << 15) / pkb) : 32768;
            if (t < lg0)
                lg0 = lg1 = t;                          /* (down at once: no sample of the block over HM_LIM) */
            else
                lg1 = lg0 + ((t - lg0) >> 7) + (t > lg0); /* (back up over ~90 ms) */
            L->lg = lg1;
        }
    mr = mw = L->mem;
    if (n > VOC_MAXN)
        n = VOC_MAXN;
    {   /* (§12 fix 1: the lane's state and the block's shared values in locals for the loop, written back after it:
         * through L-> and hb. each sample the compiler had to reload and store them around every output write. The
         * same arithmetic in the same order: the output bit for bit as before, smk_harm_test and scratchpad bx/) */
#define HM_S16 ((int32_t)(HM_SEG << 16))
#define HM_S15 ((int32_t)(HM_SEG << 15))
#define HM_DQ ((int32_t)(HM_D0 << 16))
/* one voice: its fade, (heard) its phase, the two taps' triangle crossfade, LEVEL, its pan, the drums' channel apart;
 * its meter once a pair. A voice faded out does nothing (its phase held), as before */
#define HM_VOICE(V, G, P, GT, DN, LG, PL, PR, UV) do {                                                             \
            int32_t p_, p2_, t1_, a1_, a2_, c_;                                                                     \
            G = G < GT ? (G + HM_FADE > GT ? GT : G + HM_FADE) : (G - HM_FADE < GT ? GT : G - HM_FADE);             \
            if (!G) {                                                                                               \
                if (wv)                                                                                             \
                    wv[V][i] = 0;                                                                                   \
                break;                                                                                              \
            }                                                                                                       \
            p_ = P + DN;                                                                                            \
            if (p_ < 0)                                                                                             \
                p_ += HM_S16;                                                                                       \
            else if (p_ >= HM_S16)                                                                                  \
                p_ -= HM_S16;                                                                                       \
            P = p_;                                                                                                 \
            p2_ = p_ + HM_S15;                                                                                      \
            if (p2_ >= HM_S16)                                                                                      \
                p2_ -= HM_S16;                                                                                      \
            t1_ = p_ < HM_S15 ? p_ : HM_S16 - p_;                     /* 0 .. HM_SEG << 15 */                       \
            t1_ = (int32_t)(((uint64_t)(uint32_t)t1_ * (0xFFFFFFFFu / HM_SEG)) >> 32);   /* -> Q15 */               \
            a1_ = hm_read(mr, wq, p_ + HM_DQ);                                                                      \
            a2_ = hm_read(mr, wq, p2_ + HM_DQ);                                                                     \
            c_ = (((((a1_ * t1_ + a2_ * (32767 - t1_)) >> 15) * G) >> 15) * LG) >> 15;   /* (LEVEL, the mute) */    \
            vs += c_;                                                                                               \
            if (mo)                                                   /* (mono: no pan, §13v-6) */                  \
                sl += c_;                                                                                           \
            else                                                                                                    \
                sl += (c_ * PL) >> 12, sr += (c_ * PR) >> 12;         /* (its pan) */                               \
            if (wv)                                                                                                 \
                wv[V][i] = byp ? 0 : hm_knee(c_);                     /* (WET unity, §13s) */                       \
            if (t & 1u) {                                             /* (its meter: once a pair) */                \
                c_ = c_ < 0 ? -c_ : c_;                                                                             \
                UV = c_ > UV ? c_ : UV;                                                                             \
            }                                                                                                       \
        } while (0)
        uint32_t w = L->w, t = L->t;
        int32_t half = L->half, fb = L->fb, eql = hb.eql && hm_probe != 1u, eqh = hb.eqh && hm_probe != 1u;
        int32_t gA = L->g[0], gB = L->g[1], gC = L->g[2], pA = L->ph[0], pB = L->ph[1], pC = L->ph[2];
        const int32_t gtA = L->vc[0] | L->th[0] || hm_probe == 2u ? 0 : hb.gt[0];
        const int32_t gtB = L->vc[1] | L->th[1] || hm_probe == 2u ? 0 : hb.gt[1];   /* (§13n B: a
                                                         * released voice closed in this lane fades out here; §13v-5 a
                                                         * thinned one too) */
        const int32_t gtC = L->vc[2] | L->th[2] || hm_probe == 2u ? 0 : hb.gt[2];
        const uint32_t mo = (hm_lmask >> (k % HL_N)) & 1u;   /* (§13v-6: mono while the stems stream: the voices summed,
                                                         * one output to both sides) */
        const int32_t dA = hm.dinc[0], dB = hm.dinc[1], dC = hm.dinc[2];
        const int32_t lA = hb.lg[0], lB = hb.lg[1], lC = hb.lg[2], plA = hb.pl[0], plB = hb.pl[1], plC = hb.pl[2];
        const int32_t prA = hb.pr[0], prB = hb.pr[1], prC = hb.pr[2];
        int32_t lsl = L->pvl, lsr = L->pvr;
        if (!byp && !wv && !((n | t) & 1u) && fb0 == fb1 && ig0 == ig1) {
            /* §13s: a steady block (FEEDBACK and INPUT LEVEL not ramping, IN, no voices apart), a pair at a time: the
             * loop below sample for sample, the pair's phase tests, the voices-apart and IN / OUT tests out of the loop,
             * the input's meter and the voices' sum for the feedback on the pair's second sample only (where the loop
             * below takes them); an open voice's fade (32767) without its product, the same value. LANE RATE 22K: the
             * voices once a pair, at its second sample (the feedback's), each stepping two samples; the first sample the
             * mean of the last pair's and this pair's */
            const uint32_t lim = lg0 < 32768 || lg1 < 32768, st = hm_r22 || ((hm_lrate >> (k % HL_N)) & 1u) ? 2u : 1u;
#define HM_VF(G, P, GT, DN, LG, PL, PR, UV, ST, M) do {                                                            \
                int32_t p_, p2_, t1_, a1_, a2_, c_;                                                                 \
                if (G != GT) {                                                                                      \
                    const int32_t s_ = HM_FADE * (int32_t)(ST);                                                     \
                    G = G < GT ? (G + s_ > GT ? GT : G + s_) : (G - s_ < GT ? GT : G - s_);                         \
                }                                                                                                   \
                if (!G)                                                                                             \
                    break;                                                                                          \
                p_ = P + DN * (int32_t)(ST);                                                                        \
                if (p_ < 0)                                                                                         \
                    p_ += HM_S16;                                                                                   \
                else if (p_ >= HM_S16)                                                                              \
                    p_ -= HM_S16;                                                                                   \
                P = p_;                                                                                             \
                p2_ = p_ + HM_S15;                                                                                  \
                if (p2_ >= HM_S16)                                                                                  \
                    p2_ -= HM_S16;                                                                                  \
                t1_ = p_ < HM_S15 ? p_ : HM_S16 - p_;                                                               \
                t1_ = (int32_t)(((uint64_t)(uint32_t)t1_ * (0xFFFFFFFFu / HM_SEG)) >> 32);                          \
                a1_ = hm_read(mr, wq, p_ + HM_DQ);                                                                  \
                a2_ = hm_read(mr, wq, p2_ + HM_DQ);                                                                 \
                c_ = (a1_ * t1_ + a2_ * (32767 - t1_)) >> 15;                                                       \
                c_ = G == 32767 ? ((c_ - (c_ > 0)) * LG) >> 15 : (((c_ * G) >> 15) * LG) >> 15;   /* (open: x 32767  \
                                                         * >> 15 is c_ - 1 above 0, c_ at or under: |c_| <= 32767) */ \
                if (M)                                                                                              \
                    vs += c_;                                                                                       \
                if (mo)                                                                                             \
                    sl += c_;                                                                                       \
                else                                                                                                \
                    sl += (c_ * PL) >> 12, sr += (c_ * PR) >> 12;                                                   \
                if (M) {                                                                                            \
                    c_ = c_ < 0 ? -c_ : c_;                                                                         \
                    UV = c_ > UV ? c_ : UV;                                                                         \
                }                                                                                                   \
            } while (0)
            for (i = 0; i < n; i += 2u) {
                int32_t x0 = (in[i] * ig1) >> 12, x1 = (in[i + 1u] * ig1) >> 12, vs = 0, sl = 0, sr = 0, wq, ax, ar;
                if (lim) {
                    x0 = (int32_t)(((int64_t)x0 * (lg0 + (((lg1 - lg0) * (int32_t)i) >> 5))) >> 15);
                    x1 = (int32_t)(((int64_t)x1 * (lg0 + (((lg1 - lg0) * (int32_t)(i + 1u)) >> 5))) >> 15);
                }
                x0 = clamp(x0, -32767, 32767), x1 = clamp(x1, -32767, 32767);
                ax = x1 < 0 ? -x1 : x1;
                pk = ax > pk ? ax : pk;
                ui_in = ax > ui_in ? ax : ui_in;
                if (sc) {                               /* (t even: the ring's eighth sample is never the pair's first) */
                    hm.dacc += x0 + x1;
                    if (((t + 1u) & 7u) == 7u) {
                        hm_tring[hm.tw & (HM_TRN - 1u)] = (int16_t)(hm.dacc >> 3);
                        hm.tw++;
                        hm.dacc = 0;
                    }
                }
                half = x0;
                if (st == 1u) {                         /* the pair's first sample */
                    wq = (int32_t)(w << 16) - 32768;
                    HM_VF(gA, pA, gtA, dA, lA, plA, prA, ui_v[0], 1, 0);
                    HM_VF(gB, pB, gtB, dB, lB, plB, prB, ui_v[1], 1, 0);
                    HM_VF(gC, pC, gtC, dC, lC, plC, prC, ui_v[2], 1, 0);
                    sl = clamp(sl, -98301, 98301);
                    if (mo) {
                        wl[i] = wr[i] = hm_knee(sl);
                    } else {
                        sr = clamp(sr, -98301, 98301);
                        wl[i] = hm_knee(sl), wr[i] = hm_knee(sr);
                    }
                    sl = sr = 0;
                }
                if (!rep) {                             /* its second: one stored sample, the feedback in, soft-limited */
                    int32_t s = clamp(((x0 + x1) >> 1) + fb, -32767, 32767), as = s < 0 ? -s : s;
                    ui_fb = as > ui_fb ? as : ui_fb;
                    s -= (((s * s) >> 15) * s >> 15) * 10923 >> 15;
                    mw[w] = (int16_t)s;
                    if (!w)
                        mw[HM_LEN] = (int16_t)s;        /* (the guard: slot 0's copy) */
                }
                w = w + 1u >= HM_LEN ? 0u : w + 1u;
                wq = (int32_t)(w << 16);
                HM_VF(gA, pA, gtA, dA, lA, plA, prA, ui_v[0], st, 1);
                HM_VF(gB, pB, gtB, dB, lB, plB, prB, ui_v[1], st, 1);
                HM_VF(gC, pC, gtC, dC, lC, plC, prC, ui_v[2], st, 1);
                {
                    int32_t f = clamp(vs, -65535, 65535) * fb1;
                    f = (f + (f < 0 ? 32767 : 0)) >> 15;
                    if (eql)
                        f = hm_bq(L->eq[0], hb.cq[0], f);
                    if (eqh)
                        f = hm_bq(L->eq[1], hb.cq[1], f);
                    fb = f;
                }
                sl = clamp(sl, -98301, 98301);
                if (mo) {                               /* (mono: one knee a sample, both sides) */
                    if (st != 1u)
                        wl[i] = wr[i] = hm_knee((lsl + sl) >> 1);
                    lsl = lsr = sl;
                    wl[i + 1u] = wr[i + 1u] = hm_knee(sl);
                    ax = wl[i + 1u] < 0 ? -wl[i + 1u] : wl[i + 1u];
                } else {
                    sr = clamp(sr, -98301, 98301);
                    if (st != 1u)                       /* (22K: the first sample between the pairs) */
                        wl[i] = hm_knee((lsl + sl) >> 1), wr[i] = hm_knee((lsr + sr) >> 1);
                    lsl = sl, lsr = sr;
                    wl[i + 1u] = hm_knee(sl), wr[i + 1u] = hm_knee(sr);
                    ax = wl[i + 1u] < 0 ? -wl[i + 1u] : wl[i + 1u];
                    ar = wr[i + 1u] < 0 ? -wr[i + 1u] : wr[i + 1u];
                    ax = ax > ar ? ax : ar;
                }
                po = ax > po ? ax : po;
                ui_out = ax > ui_out ? ax : ui_out;
                t += 2u;
            }
#undef HM_VF
        } else
        for (i = 0; i < n; i++) {
            int32_t x, vs = 0, sl = 0, sr = 0, wq, ax;
            fbk = fb0 + (((fb1 - fb0) * (int32_t)i) >> 5);   /* (§13c: across the block, VOC_MAXN = 32; at rest the same
                                                         * value as before, bit for bit) */
            ig = ig0 + (((ig1 - ig0) * (int32_t)i) >> 5);
            x = (in[i] * ig) >> 12;                      /* (INPUT LEVEL) */
            if (lg0 < 32768 || lg1 < 32768)             /* (§13q: the limiter, only while it acts) */
                x = (int32_t)(((int64_t)x * (lg0 + (((lg1 - lg0) * (int32_t)i) >> 5))) >> 15);
            x = clamp(x, -32767, 32767);
            if (t & 1u) {                               /* (the meters: once a pair, §12) */
                ax = x < 0 ? -x : x;
                pk = ax > pk ? ax : pk;
                ui_in = ax > ui_in ? ax : ui_in;
            }
            if (sc) {
                hm.dacc += x;                           /* the tracker's dry ring: an 8-sample mean, 5.5 kHz */
                if ((t & 7u) == 7u) {
                    hm_tring[hm.tw & (HM_TRN - 1u)] = (int16_t)(hm.dacc >> 3);
                    hm.tw++;
                    hm.dacc = 0;
                }
            }
            if (!(t & 1u)) {
                half = x;
            } else {                                    /* a pair: one stored sample, the feedback in, soft-limited */
                if (!rep) {
                    int32_t s = clamp(((half + x) >> 1) + fb, -32767, 32767), as = s < 0 ? -s : s;
                    ui_fb = as > ui_fb ? as : ui_fb;
                    s -= (((s * s) >> 15) * s >> 15) * 10923 >> 15;   /* x - x^3 / 3 (slope <= 1; full scale -> 2/3) */
                    mw[w] = (int16_t)s;
                    if (!w)
                        mw[HM_LEN] = (int16_t)s;        /* (the guard: slot 0's copy, §13s) */
                }
                w = w + 1u >= HM_LEN ? 0u : w + 1u;
            }
            wq = (int32_t)(w << 16) - (int32_t)((~t & 1u) << 15);   /* the newest slot, the half-pair's phase */
            HM_VOICE(0, gA, pA, gtA, dA, lA, plA, prA, ui_v[0]);   /* the voices: two taps, a triangle crossfade */
            HM_VOICE(1, gB, pB, gtB, dB, lB, plB, prB, ui_v[1]);
            HM_VOICE(2, gC, pC, gtC, dC, lC, plC, prC, ui_v[2]);
            if (t & 1u) {                               /* (22.05 kHz: the next pair's), rounded toward 0 */
                int32_t f = clamp(vs, -65535, 65535) * fbk;
                f = (f + (f < 0 ? 32767 : 0)) >> 15;
                if (eql)
                    f = hm_bq(L->eq[0], hb.cq[0], f);
                if (eqh)
                    f = hm_bq(L->eq[1], hb.cq[1], f);
                fb = f;
            }
            if (byp) {                                  /* IN / OUT: OUT, no wet (the dry, hb.kd 32768, is the input) */
                wl[i] = wr[i] = 0;
                lsl = lsr = 0;
            } else {                                    /* (WET unity, §13s: no product; it was sl x 32768 in 32 bits,
                                                         * which wrapped past 65535) */
                sl = clamp(sl, -98301, 98301);
                if (mo) {
                    wl[i] = wr[i] = hm_knee(sl);
                    lsl = lsr = sl;
                } else {
                    sr = clamp(sr, -98301, 98301);
                    wl[i] = hm_knee(sl), wr[i] = hm_knee(sr);
                    lsl = sl, lsr = sr;
                }
            }
            if (t & 1u) {                               /* (the meters: once a pair) */
                int32_t ar = wr[i] < 0 ? -wr[i] : wr[i];
                ax = wl[i] < 0 ? -wl[i] : wl[i];
                ax = ax > ar ? ax : ar;
                po = ax > po ? ax : po;
                ui_out = ax > ui_out ? ax : ui_out;
            }
            t++;
        }
        L->w = w, L->t = t, L->half = half, L->fb = fb, L->pvl = lsl, L->pvr = lsr;
        L->g[0] = gA, L->g[1] = gB, L->g[2] = gC, L->ph[0] = pA, L->ph[1] = pB, L->ph[2] = pC;
    }
#undef HM_VOICE
#undef HM_S16
#undef HM_S15
#undef HM_DQ
    }
 {
        int32_t af = L->fb < 0 ? -L->fb : L->fb;        /* (§13f: this block silent throughout? a count toward sleep) */
        for (v = 0; v < 3u; v++)                        /* (§13n B: a released voice's own output here, quiet for
                                                         * HM_RQB blocks: closed in this lane) */
            if (hb.rl[v] && !L->vc[v]) {
                L->vq[v] = (uint8_t)(ui_v[v] <= HM_QTHR ? L->vq[v] + 1u : 0u);
                if (L->vq[v] >= HM_RQB)
                    L->vc[v] = 1;
            }
        for (v = 0; v < 3u; v++)                        /* (§13v-5: each voice's level here, for thinning's choice) */
            L->vlv[v] = (uint16_t)(ui_v[v] > 65535 ? 65535 : ui_v[v]);
        if (!rep && ui_in <= HM_QTHR && ui_out <= HM_QTHR && af <= HM_QTHR && ui_v[0] <= HM_QTHR && ui_v[1] <= HM_QTHR &&
            ui_v[2] <= HM_QTHR)
            L->quiet = (uint16_t)(L->quiet < 0xFFFFu ? L->quiet + 1u : L->quiet);
        else
            L->quiet = 0;
    }
    hm.pk_in = pk, hm.pk_out = po;
    if (ui_in > hm.up[HM_UP_IN])                        /* (the page's, the loudest lane's: it takes them at its rate) */
        hm.up[HM_UP_IN] = ui_in;
    for (v = 0; v < 3u; v++)
        if (ui_v[v] > hm.up[HM_UP_V + v])
            hm.up[HM_UP_V + v] = ui_v[v];
    if (ui_out > hm.up[HM_UP_OUT])
        hm.up[HM_UP_OUT] = ui_out;
    if (ui_fb > hm.up[HM_UP_FB])
        hm.up[HM_UP_FB] = ui_fb;
    if (ui_in > hm.lpi[k % HL_N])                       /* (each lane's, the console's) */
        hm.lpi[k % HL_N] = ui_in;
    if (ui_out > hm.lpo[k % HL_N])
        hm.lpo[k % HL_N] = ui_out;
    if (ui_fb > hm.lpf[k % HL_N])
        hm.lpf[k % HL_N] = ui_fb;
    if (k == HL_VOICE) {
        if (sc) {
            TIM_VOC_T(tt);
            hm_lags();
            TIM_ADD(TS_TRK, TIM_NOW() - tt);
        } else {
            hm.tnote = 0, hm.p1 = hm.p2 = 0, hm.lag = 0;   /* (SCALE off: the tracker at rest, a fresh start later) */
        }
    }
    TIM_ADD(TS_LV + k % HL_N, TIM_NOW() - tl);
    return 1u;
}
/* thinning (harmoniser-spec.md §13v-5), audio.c on an overload (two halves in a row over 85 %): a lane voice faded out
 * before a synth voice is shed: in the first lane (T3, T2, VOICE, then DRUMS) with LANE THIN on that runs and has a
 * voice open, its quietest voice. 1: one thinned */
static uint32_t hm_thin_one(void)
{
    static const uint8_t ORD[HL_N] = {HL_T3, HL_T2, HL_VOICE, HL_DRUM};
    uint32_t j, v;
    if (!hm_on)
        return 0;
    for (j = 0; j < HL_N; j++) {
        hm_lane_t *L = &hm_ln[ORD[j]];
        uint32_t best = 3u;
        if (!((hm_thin >> ORD[j]) & 1u) || !L->run || !L->blk || hv.nblk - L->blk > 2u || L->quiet >= HM_QBLK)
            continue;
        for (v = 0; v < 3u; v++)
            if (hb.gt[v] && !L->vc[v] && !L->th[v] && (best == 3u || L->vlv[v] < L->vlv[best]))
                best = v;
        if (best < 3u) {
            L->th[best] = 1;
            hv.thins++;
            return 1;
        }
    }
    return 0;
}
/* .. and back (audio.c, once the halves have stayed under 70 % for ~0.5 s): one voice, the last lanes thinned first */
static void hm_unthin_one(void)
{
    static const uint8_t ORD[HL_N] = {HL_DRUM, HL_VOICE, HL_T2, HL_T3};
    uint32_t j, v;
    for (j = 0; j < HL_N; j++)
        for (v = 0; v < 3u; v++)
            if (hm_ln[ORD[j]].th[v]) {
                hm_ln[ORD[j]].th[v] = 0;
                hv.unthins++;
                return;
            }
}
/* T1's engine while HARMONY is on (voice.c track_render): the input routed to HARMONY at VOICE IN, n <= VOC_MAXN samples
 * on the track's scale; its chain (DIST, SLICER, LEVEL, the punch-in) then the VOICE lane (fx.c hm_insert); 1 (the chain
 * runs) */
static uint32_t __attribute__((noinline)) hm_render(track_t *t, int32_t *out, uint32_t n)
{
    uint32_t i;
    static int32_t vgl = -1;                           /* VOICE IN at the last block's end (§13c: ramped) */
    int32_t vg = VOC_AUDIO_ON ? HM_GAIN(hm_send[0]) : 0, v0;   /* VOICE IN (AUDIO IN off: silent) */
    const int16_t *in = uin_dst[UD_HARM];
    (void)t;
    if (vgl < 0)
        vgl = vg;
    v0 = vgl, vgl = vg;
    if (n > VOC_MAXN)
        n = VOC_MAXN;
    for (i = 0; i < n; i++)
        out[i] = VOC_VS((in[i] * (v0 + (((vg - v0) * (int32_t)i) >> 5))) >> 15);
    return 1u;
}
