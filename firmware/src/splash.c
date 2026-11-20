/* SPDX-License-Identifier: GPL-3.0-only */
/* SLOOP boot splash: the logo (tools/gen_logo.py -> sloop_logo.h: 16 colours, RLE runs of
 * (run - 1) << 4 | colour), drawn into the canvas in bands of at most 120 rows. */
#ifndef UI_SMALL                 /* (2.7) compiled for size: not the audio path (gfx.c UI_SMALL); the host tests: nothing */
#define UI_SMALL
#endif
#ifndef FELUCCA_LOGO_TEXT       /* (2.7) 1: the boot logo as text, "S" and "LOOP" in the four track colours (4.6 KB of flash
                                 * less: tools/build.py builds so by itself when the flash is full) */
#define FELUCCA_LOGO_TEXT 0
#endif
#if FELUCCA_LOGO_TEXT
UI_SMALL static void sloop_logo_draw(uint32_t y0)
{
    static const char L[5] = {'S', 'L', 'O', 'O', 'P'};
    uint32_t i, w = text_w(&FONT_L, "SLOOP"), k = w * 3u <= 232u ? 3u : 2u, h = 32u, off, y, x;
    int32_t px = 0;
    char c[2] = {0, 0};
    cv_begin(240, h * k, C_BLACK);
    for (i = 0; i < 5u; i++) {                          /* small, at the top left: S white, L O O P the tracks' colours */
        c[0] = L[i];
        px = cv_text(px, 0, &FONT_L, c, i ? TE_COL_DEF[i - 1u] : C_WHITE);
    }
    off = (240u - w * k) / 2u;
    for (y = h * k; y-- > 0;)                           /* then k times as big, in place (from the end: nothing read is
                                                         * overwritten before) */
        for (x = w * k; x-- > 0;)
            cv_px[y * 240u + off + x] = cv_px[(y / k) * 240u + x / k];
    for (y = 0; y < h * k; y++)                         /* (the small one's left-over columns) */
        for (x = 0; x < off; x++)
            cv_px[y * 240u + x] = swap16(C_BLACK);
    cv_blit(0, y0 + 60u);
}
#else
#include "sloop_logo.h"
UI_SMALL static void sloop_logo_draw(uint32_t y0)
{
    uint32_t r0;
    for (r0 = 0; r0 < SLOOP_SPLASH_H; r0 += 120u) {
        uint32_t rows = SLOOP_SPLASH_H - r0 > 120u ? 120u : SLOOP_SPLASH_H - r0;
        uint32_t lo = r0 * SLOOP_SPLASH_W, hi = (r0 + rows) * SLOOP_SPLASH_W, pos = 0, i;
        cv_begin(SLOOP_SPLASH_W, rows, C_BLACK);
        for (i = 0; i < sizeof SLOOP_SPLASH_RLE && pos < hi; i++) {
            uint32_t run = (SLOOP_SPLASH_RLE[i] >> 4) + 1u, c = SLOOP_SPLASH_RLE[i] & 15u, p;
            if (c && pos + run > lo)
                for (p = pos < lo ? lo : pos; p < pos + run && p < hi; p++)
                    cv_px[p - lo] = swap16(SLOOP_SPLASH_PAL[c]);
            pos += run;
        }
        cv_blit((240u - SLOOP_SPLASH_W) / 2u, y0 + r0);
    }
}
#endif

#if BOARD_SMK37
/* SMK-37: the dancing cats (smk37-devtools splash-spec.md): the owner's hand-drawn line animation as vector strokes
 * (tools/gen_splash_anim.py -> splash_anim.h, 10 frames, ~12.7 KB), drawn by the unit in place of the SLOOP logo: at
 * power-on the first frame, then (main.c, once the keys are read) frame after frame at 250 ms until a key, pad or
 * button is pressed (or 10 s). Under them the build's name and its version (the "+" and the version in magenta). */
#include "splash_anim.h"
#ifndef SPLASH_AA
#define SPLASH_AA 0                 /* the lines: 0 plain 1-px (gfx.c cv_line: the owner's choice, crisp), 1 antialiased
                                     * (soft, ~2 px), 2 a sharp core with faint steps (smk37-devtools splash-spec.md) */
#endif
#define SPLASH_Y 4u                 /* the cats' box: its top row on the screen, centred across */
#define SPLASH_X ((240u - SPLASH_ANIM_W) / 2u)
#define SPLASH_BAND 98u             /* drawn in two bands (the canvas holds 240 x 124) */
#define SPLASH_CAP_MS 10000u
#define SPLASH_MAGENTA RGB(255, 64, 255)

/* frame f's data: the stroke count's 2 bytes, then each stroke: colour << 6, points, x0, y0, then a step a byte
 * ((dx + 8) << 4 | (dy + 8)) or 0x00, dx, dy */
UI_SMALL static const uint8_t *splash_frame(uint32_t f)
{
    const uint8_t *p = SPLASH_ANIM;
    while (f--) {
        uint32_t n = p[0] | (uint32_t)p[1] << 8, s, j;
        p += 2;
        for (s = 0; s < n; s++) {
            uint32_t k = p[1];
            p += 4;
            for (j = 1; j < k; j++)
                p += *p ? 1u : 3u;
        }
    }
    return p;
}

#if SPLASH_AA
/* a pixel at a (0 .. 256) of c, kept where the canvas is already brighter (lines meet without dark seams) */
UI_SMALL static void splash_px(int32_t x, int32_t y, uint32_t c, uint32_t a)
{
    uint32_t o, r, g, b;
    x += (int32_t)SPLASH_X, y += cv_oy;
    if (!a || (uint32_t)x >= cv_w || (uint32_t)y >= cv_h)
        return;
#if SPLASH_AA == 2                                  /* a sharp core: the nearer pixel whole, the other faint */
    a = a >= 128u ? 256u : a * 3u / 4u;
#else
    a = a * 3u / 2u > 256u ? 256u : a * 3u / 2u;      /* (x 1.5: a line split over two pixels keeps its weight) */
#endif
    o = swap16(cv_px[(uint32_t)y * cv_w + (uint32_t)x]);
    r = ((c >> 11) & 31u) * a >> 8, g = ((c >> 5) & 63u) * a >> 8, b = (c & 31u) * a >> 8;
    r = r > ((o >> 11) & 31u) ? r : ((o >> 11) & 31u);
    g = g > ((o >> 5) & 63u) ? g : ((o >> 5) & 63u);
    b = b > (o & 31u) ? b : (o & 31u);
    cv_px[(uint32_t)y * cv_w + (uint32_t)x] = swap16(r << 11 | g << 5 | b);
}
#endif

/* a line of the cats (box coordinates): antialiased (Wu: each step's two pixels by its fraction), or cv_line */
UI_SMALL static void splash_line(int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint32_t c)
{
#if SPLASH_AA
    int32_t dx = x1 - x0, dy = y1 - y0, ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy, i, f, g;
    if (ax >= ay) {
        if (!ax) {
            splash_px(x0, y0, c, 256u);
            return;
        }
        if (dx < 0) {
            i = x0, x0 = x1, x1 = i, i = y0, y0 = y1, y1 = i, dy = -dy;
        }
        g = (dy * 65536) / ax, f = y0 * 65536 + 32768;
        for (i = x0; i <= x1; i++, f += g) {
            int32_t yi = f >> 16, fr = (f >> 8) & 255;
            splash_px(i, yi, c, (uint32_t)(256 - fr));
            splash_px(i, yi + 1, c, (uint32_t)fr);
        }
    } else {
        if (dy < 0) {
            i = x0, x0 = x1, x1 = i, i = y0, y0 = y1, y1 = i, dx = -dx;
        }
        g = (dx * 65536) / ay, f = x0 * 65536 + 32768;
        for (i = y0; i <= y1; i++, f += g) {
            int32_t xi = f >> 16, fr = (f >> 8) & 255;
            splash_px(xi, i, c, (uint32_t)(256 - fr));
            splash_px(xi + 1, i, c, (uint32_t)fr);
        }
    }
#else
    cv_line(x0 + (int32_t)SPLASH_X, y0, x1 + (int32_t)SPLASH_X, y1, (uint16_t)c);
#endif
}

UI_SMALL static void splash_frame_draw(uint32_t f)
{
    uint32_t r0;
    for (r0 = 0; r0 < SPLASH_ANIM_H; r0 += SPLASH_BAND) {
        uint32_t rows = SPLASH_ANIM_H - r0 > SPLASH_BAND ? SPLASH_BAND : SPLASH_ANIM_H - r0, n, s, j;
        const uint8_t *p = splash_frame(f);
        cv_begin(240, rows, C_BLACK);
        cv_oy = -(int32_t)r0;
        n = p[0] | (uint32_t)p[1] << 8;
        p += 2;
        for (s = 0; s < n; s++) {
            uint32_t c = SPLASH_ANIM_PAL[p[0] >> 6], k = p[1];
            int32_t x = p[2], y = p[3];
            p += 4;
            if (k == 1u)
                splash_line(x, y, x, y, c);
            for (j = 1; j < k; j++) {
                int32_t dx, dy;
                if (*p) {
                    dx = (int32_t)(*p >> 4) - 8, dy = (int32_t)(*p & 15u) - 8;
                    p++;
                } else {
                    dx = (int8_t)p[1], dy = (int8_t)p[2];
                    p += 3;
                }
                splash_line(x, y, x + dx, y + dy, c);
                x += dx, y += dy;
            }
        }
        cv_oy = 0;
        cv_blit(0, SPLASH_Y + r0);
    }
}

/* the boot screen (SMK-37): the cats' first frame; the build's name ("+" magenta), its version (magenta), Felucca */
UI_SMALL static void sloop_splash(void)
{
    static char b[40];
    const char *v = FELUCCA_VERSION;
#if SMK_UAC_IN_PROBE && defined(SMK_HARM)
    static const char *const T[3] = {"MEWCODER ", "+", " HARMONISER"};
#elif SMK_UAC_IN_PROBE
    static const char *const T[3] = {"MEWCODER", "", ""};
#else
    static const char *const T[3] = {"MULTICHANNEL", "", ""};
#endif
    uint32_t k = 0, i, w = 0;
    int32_t x;
    lcd_fill(0, 0, 240, 240, C_BLACK);
    splash_frame_draw(0);
    if (v[0] == 'S' && v[1] == 'L' && v[5] == ' ')
        v += 6;                                         /* "SLOOP 2.8.4" -> "2.8.4" */
    {
        const char *parts[2] = {v, SMK_BUILD_ID};
        for (i = 0; i < 2u; i++) {
            const char *q = parts[i];
            if (k && k < sizeof b - 3u)
                b[k++] = ' ', b[k++] = ' ';
            while (*q && k < sizeof b - 1u)
                b[k++] = *q++;
        }
        b[k] = 0;
    }
    cv_begin(240, 36, C_BLACK);
    for (i = 0; i < 3u; i++)
        w += text_w(&FONT_S, T[i]);
    x = 120 - (int32_t)w / 2;
    for (i = 0; i < 3u; i++)
        x = cv_text(x, 0, &FONT_S, T[i], i == 1u ? SPLASH_MAGENTA : RGB(196, 196, 204));
    w = text_w(&FONT_S, b) + text_w(&FONT_S, "  felucca");
    x = cv_text(120 - (int32_t)w / 2, 18, &FONT_S, b, SPLASH_MAGENTA);
    cv_text(x, 18, &FONT_S, "  felucca", RGB(96, 96, 104));
    cv_blit(0, 202);
}

/* main.c, the keys read: the cats dance until a key, a pad or a button is pressed (or 10 s); then the press is let
 * go of first (up to 2 s) and its edges cleared, so it plays nothing */
static void splash_play(void)
{
    uint32_t t0 = fm1_ms, next = fm1_ms + SPLASH_ANIM_MS, f = 1;
    for (;;) {
        fm1_wdt_feed();
        usb_retry(fm1_ms);
        if (fm1_in.notes | fm1_in.buttons | (uint32_t)fm1_in.keys | (uint32_t)(fm1_in.keys >> 32))
            break;
        if ((uint32_t)(fm1_ms - t0) >= SPLASH_CAP_MS)
            break;
        if ((int32_t)(fm1_ms - next) >= 0) {
            splash_frame_draw(f);
            f = (f + 1u) % SPLASH_ANIM_N;
            next += SPLASH_ANIM_MS;
        }
    }
    t0 = fm1_ms;
    while ((fm1_in.notes | fm1_in.buttons | (uint32_t)fm1_in.keys | (uint32_t)(fm1_in.keys >> 32)) &&
           (uint32_t)(fm1_ms - t0) < 2000u) {
        fm1_wdt_feed();
        usb_retry(fm1_ms);
    }
    fm1_in.notes_pressed = 0, fm1_in.pressed = 0, fm1_in.released = 0;
}
#else
/* the boot screen: the logo, the version, where it comes from */
UI_SMALL static void sloop_splash(void)
{
    lcd_fill(0, 0, 240, 240, C_BLACK);
    sloop_logo_draw(10);
    const char *v = FELUCCA_VERSION;
    if (v[0] == 'S' && v[1] == 'L' && v[5] == ' ')
        v += 6;                                         /* "SLOOP 1.0" -> "1.0" under the wordmark */
    cv_begin(240, 36, C_BLACK);
#ifdef SMK_BUILD_ID
    {   /* SMK-37: the version, the commit built and the variant ("2.8.4  aa42b0e  VOCODER"): which build is on it */
        static char b[40];
        uint32_t k = 0, i;
        const char *parts[3] = {v, SMK_BUILD_ID, SMK_UAC_IN_PROBE ? "VOCODER" : ""};
        for (i = 0; i < 3u; i++) {
            const char *q = parts[i];
            if (!*q)
                continue;
            if (k && k < sizeof b - 3u)
                b[k++] = ' ', b[k++] = ' ';
            while (*q && k < sizeof b - 1u)
                b[k++] = *q++;
        }
        b[k] = 0;
        v = b;
    }
#endif
    cv_text(120 - text_w(&FONT_S, v) / 2, 0, &FONT_S, v, RGB(196, 196, 204));
    cv_text(120 - text_w(&FONT_S, "based on felucca") / 2, 18, &FONT_S, "based on felucca", RGB(96, 96, 104));
    cv_blit(0, 202);
}
#endif
