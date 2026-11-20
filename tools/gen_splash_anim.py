#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""The SMK-37 boot splash's dancing cats (smk37-devtools splash-spec.md): the owner's hand-drawn line GIF
(assets/splash/kitty_CY.gif, 10 frames; its frames run in reverse, Layer-10 .. Layer-1, so they are taken last to
first) into vector strokes for the unit to draw: each frame's lines thinned to their centre, traced into strokes,
simplified to within EPS px at the drawn size, each stroke one colour of a small palette.

  tools/gen_splash_anim.py OUT.h                 the header for firmware/src/splash.c
  tools/gen_splash_anim.py --preview DIR         preview PNGs / GIFs: the unit's 1-px lines and an antialiased version

Encoding (per frame): stroke count; per stroke: colour index, point count, the first point (x, y bytes), then each
point as a signed (dx, dy) byte pair (a step over 127 is split). Coordinates in the drawn box (SPLASH_W x SPLASH_H)."""
import sys
from pathlib import Path
import numpy as np
from PIL import Image, ImageSequence, ImageDraw

HERE = Path(__file__).resolve().parent
SRC = HERE.parent / "assets" / "splash" / "kitty_CY.gif"
SPLASH_H = 196                  # the drawn box's height on the 240 x 240 screen (the version lines below it)
EPS = 0.8                       # simplification tolerance, px at the drawn size
MS = 250                        # a frame (the GIF's timing)


def thin(img):
    """Zhang-Suen thinning: bool image -> 1-px centre lines"""
    im = img.astype(np.uint8).copy()
    while True:
        changed = False
        for step in (0, 1):
            p = np.pad(im, 1)
            P2, P3, P4, P5 = p[:-2, 1:-1], p[:-2, 2:], p[1:-1, 2:], p[2:, 2:]
            P6, P7, P8, P9 = p[2:, 1:-1], p[2:, :-2], p[1:-1, :-2], p[:-2, :-2]
            nb = [P2, P3, P4, P5, P6, P7, P8, P9]
            B = sum(n.astype(np.int16) for n in nb)
            A = sum(((nb[k] == 0) & (nb[(k + 1) % 8] == 1)).astype(np.int16) for k in range(8))
            c = ((P2 * P4 * P6 == 0) & (P4 * P6 * P8 == 0)) if step == 0 else ((P2 * P4 * P8 == 0) & (P2 * P6 * P8 == 0))
            rm = (im == 1) & (B >= 2) & (B <= 6) & (A == 1) & c
            if rm.any():
                im[rm] = 0
                changed = True
        if not changed:
            return im.astype(bool)


def chains(sk):
    """1-px lines -> ordered point lists (ends first; straight on through a branch)"""
    pts = set(zip(*[a.tolist() for a in np.nonzero(sk)]))
    nb = [(-1, -1), (-1, 0), (-1, 1), (0, -1), (0, 1), (1, -1), (1, 0), (1, 1)]

    def nbrs(q):
        return [(q[0] + a, q[1] + b) for a, b in nb if (q[0] + a, q[1] + b) in pts]
    out = []
    for s in sorted(pts, key=lambda q: (len(nbrs(q)), q)):
        if s not in pts:
            continue
        path = [s]
        pts.discard(s)
        cur = s
        while True:
            n = nbrs(cur)
            if not n:
                break
            if len(path) > 1:
                dy, dx = cur[0] - path[-2][0], cur[1] - path[-2][1]
                n.sort(key=lambda q: -((q[0] - cur[0]) * dy + (q[1] - cur[1]) * dx))
            cur = n[0]
            path.append(cur)
            pts.discard(cur)
        out.append(np.array([(x, y) for y, x in path], np.float32))
    return out


def simplify(p, eps):
    """Douglas-Peucker on an open polyline"""
    if len(p) < 3:
        return p
    a, b = p[0], p[-1]
    ab = b - a
    n = np.hypot(*ab)
    r = p - a
    d = np.abs(ab[0] * r[:, 1] - ab[1] * r[:, 0]) / n if n > 1e-6 else np.hypot(*r.T)
    i = int(np.argmax(d))
    if d[i] <= eps:
        return np.array([a, b])
    return np.vstack([simplify(p[: i + 1], eps)[:-1], simplify(p[i:], eps)])


def frames():
    g = Image.open(SRC)
    fr = [f.convert("RGBA") for f in ImageSequence.Iterator(g)]
    return fr[::-1]                      # the clip's order: the GIF runs Layer-10 .. Layer-1 (frames 45 .. 0)


def palette(fr):
    """the four colours of the owner's drawing: the yellow cat, the cyan cat, their eyes (orange, light blue)"""
    return [np.array(c, np.float32) for c in ((246, 246, 17), (26, 214, 234), (253, 204, 91), (66, 199, 253))]


def eye_mask(a):
    r, g, b = [a[..., k].astype(int) for k in range(3)]
    on = a[..., 3] > 70
    return on & (((b > r + 80) & (b > g + 20)) | ((r > b + 80) & (g < r - 25)))


def join(strokes, tol):
    """fragments whose ends meet (within tol px) joined end to end, the same colour only: fewer, longer strokes"""
    strokes = [[ci, list(map(tuple, p))] for ci, p in strokes]
    merged = True
    while merged:
        merged = False
        ends = {}
        for k, (ci, p) in enumerate(strokes):
            for e, q in ((0, p[0]), (1, p[-1])):
                ends.setdefault((ci, q[0] // 4, q[1] // 4), []).append((k, e))
        used = set()
        out = []
        for k, (ci, p) in enumerate(strokes):
            if k in used:
                continue
            used.add(k)
            grew = True
            while grew:
                grew = False
                for e in (1, 0):
                    q = p[-1] if e else p[0]
                    best = None
                    for gx in (-1, 0, 1):
                        for gy in (-1, 0, 1):
                            for (k2, e2) in ends.get((ci, q[0] // 4 + gx, q[1] // 4 + gy), []):
                                if k2 in used:
                                    continue
                                r = strokes[k2][1][-1 if e2 else 0]
                                dd = (r[0] - q[0]) ** 2 + (r[1] - q[1]) ** 2
                                if dd <= tol * tol and (best is None or dd < best[0]):
                                    best = (dd, k2, e2)
                    if best:
                        _, k2, e2 = best
                        used.add(k2)
                        seg = strokes[k2][1] if e2 == 0 else strokes[k2][1][::-1]   # seg starts at the meeting end
                        if e:
                            p = p + seg
                        else:
                            p = seg[::-1] + p
                        grew = merged = True
            out.append([ci, p])
        strokes = out
    return [(ci, np.array(p, np.float32)) for ci, p in strokes]


def vectorise(f, pal):
    W0, H0 = f.size
    sc = SPLASH_H / H0
    W = int(round(W0 * sc))
    S = 2
    big = f.resize((W * S, SPLASH_H * S), Image.LANCZOS)
    a = np.asarray(big)
    import cv2
    line = a[..., 3] > 70
    # the solid shapes (the eyes, a nose: thicker than a line, whatever their colour) and the eye colours
    solid = cv2.morphologyEx(line.astype(np.uint8), cv2.MORPH_OPEN,
                             cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (4, 4))).astype(bool)
    eyes = solid | (eye_mask(a) & cv2.dilate(solid.astype(np.uint8), np.ones((5, 5))).astype(bool))
    on = line & ~cv2.dilate(eyes.astype(np.uint8), np.ones((3, 3))).astype(bool)
    sk = thin(on)
    raw = []
    n, lab, st, _ = cv2.connectedComponentsWithStats(eyes.astype(np.uint8), 8)
    eye_strokes = []
    for k in range(1, n):
        if st[k, cv2.CC_STAT_AREA] < 6:
            continue
        reg = lab == k
        col = a[reg][:, :3].astype(np.float32).mean(0)
        ci = int(np.argmin([np.abs(col - p).sum() for p in pal]))
        cs, _ = cv2.findContours(reg.astype(np.uint8), cv2.RETR_EXTERNAL, cv2.CHAIN_APPROX_NONE)
        c = max(cs, key=len)[:, 0, :].astype(np.float32) / S
        c = np.vstack([c, c[:1]])
        pts = simplify(c, 0.6)
        if len(pts) < 3:
            pts = np.vstack([c[0], c[len(c) // 2], c[0]])
        eye_strokes.append((ci, np.round(pts).astype(int)))
    for ch in chains(sk):
        xs, ys = ch[:, 0].astype(int), ch[:, 1].astype(int)
        col = a[ys, xs, :3].astype(np.float32).mean(0)
        ci = int(np.argmin([np.abs(col - p).sum() for p in pal]))
        raw.append((ci, ch))
    raw = join(raw, 2.5 * S)
    strokes = []
    for ci, ch in raw:
        L = float(np.sum(np.hypot(*np.diff(ch, axis=0).T))) if len(ch) > 1 else 0.0
        if L < 1.5 * S and ci < 2:          # a lone speck of line (not an eye's colour): dropped
            continue
        pts = simplify(ch / S, EPS)
        if len(pts) == 1 or (len(pts) == 2 and np.allclose(pts[0], pts[1])):
            pts = np.array([pts[0], pts[0] + [1, 0]])     # a dot (an eye) as a 1-px stroke
        strokes.append((ci, np.round(pts).astype(int)))
    return W, strokes + eye_strokes


def encode(fr_strokes):
    """per frame: stroke count (2 B); per stroke: colour << 6 | 0 .. (1 B: colour in the top two bits), point count,
    x0, y0; then each step: one byte (dx + 8) << 4 | (dy + 8) when both are within -7 .. 7, else 0x00, dx, dy (signed)"""
    out = bytearray()
    for strokes in fr_strokes:
        body = bytearray()
        n = 0
        for ci, pts in strokes:
            seq = [np.asarray(pts[0], int)]
            for q in pts[1:]:
                d = np.asarray(q, int) - seq[-1]
                k = max(1, int(np.ceil(np.abs(d).max() / 127)))
                for j in range(1, k + 1):
                    seq.append(seq[0] * 0 + np.round(seq[-1] + d / k).astype(int) if j < k else np.asarray(q, int))
            seq = seq[:255]
            steps = bytearray()
            for s0, s1 in zip(seq, seq[1:]):
                dx, dy = int(s1[0] - s0[0]), int(s1[1] - s0[1])
                if -7 <= dx <= 7 and -7 <= dy <= 7 and (dx, dy) != (-8, -8):
                    steps.append(((dx + 8) << 4) | (dy + 8))
                else:
                    steps += bytes([0, dx & 255, dy & 255])
            body += bytes([ci << 6, len(seq), int(seq[0][0]) & 255, int(seq[0][1]) & 255]) + steps
            n += 1
        out += bytes([n & 255, n >> 8]) + body
    return out


def draw(W, strokes, pal, aa, scale=1):
    """the frame as the unit draws it: 1-px lines (cv_line) on black, or antialiased (aa)"""
    if aa:
        im = Image.new("RGB", (W * 4 * scale, SPLASH_H * 4 * scale), (0, 0, 0))
        d = ImageDraw.Draw(im)
        for ci, pts in strokes:
            d.line([tuple(map(float, (p + 0.5) * 4 * scale)) for p in pts], fill=tuple(int(v) for v in pal[ci]), width=max(1, int(1.25 * 4 * scale)))
        return im.resize((W * scale, SPLASH_H * scale), Image.LANCZOS)
    im = Image.new("RGB", (W, SPLASH_H), (0, 0, 0))
    px = im.load()
    for ci, pts in strokes:
        c = tuple(int(v) & 0xF8 for v in pal[ci])                 # (RGB565's 5 bits)
        for (x0, y0), (x1, y1) in zip(pts, pts[1:]):
            dx, sx = abs(x1 - x0), 1 if x0 < x1 else -1
            dy, sy = -abs(y1 - y0), 1 if y0 < y1 else -1
            err = dx + dy
            while True:
                if 0 <= x0 < W and 0 <= y0 < SPLASH_H:
                    px[int(x0), int(y0)] = c
                if x0 == x1 and y0 == y1:
                    break
                e2 = 2 * err
                if e2 >= dy:
                    err += dy; x0 += sx
                if e2 <= dx:
                    err += dx; y0 += sy
    return im.resize((W * scale, SPLASH_H * scale), Image.NEAREST) if scale > 1 else im


def screen(frame_img, W, title="MEWCODER + HARMONISER", ver="2.8.4  a8b1ae9"):
    """the 240 x 240 screen: the cats centred at the top, the two text lines under them"""
    s = Image.new("RGB", (240, 240), (0, 0, 0))
    s.paste(frame_img, ((240 - W) // 2, 2))
    d = ImageDraw.Draw(s)
    for y, t, c in ((204, title, (196, 196, 204)), (221, ver, (96, 96, 104))):
        w = d.textlength(t)
        d.text(((240 - w) / 2, y), t, fill=c)
    return s


def main():
    fr = frames()
    pal = palette(fr)
    vec = [vectorise(f, pal) for f in fr]
    W = vec[0][0]
    blob = encode([s for _, s in vec])
    npts = sum(len(p) for _, s in vec for _, p in s)
    nstr = sum(len(s) for _, s in vec)
    print(f"splash: {len(fr)} frames, {W} x {SPLASH_H}, {nstr} strokes, {npts} points, palette {[tuple(int(v) for v in p) for p in pal]}, "
          f"{len(blob)} B")
    if len(sys.argv) == 3 and sys.argv[1] == "--preview":
        out = Path(sys.argv[2]); out.mkdir(parents=True, exist_ok=True)
        for aa in (False, True):
            tag = "aa" if aa else "1px"
            scr = [screen(draw(W, s, pal, aa), W) for _, s in vec]
            big = [x.resize((480, 480), Image.NEAREST) for x in scr]
            big[0].save(out / f"splash_{tag}.gif", save_all=True, append_images=big[1:], duration=MS, loop=0)
            big[0].save(out / f"splash_{tag}_frame0.png")
        return
    path = Path(sys.argv[1])
    rgb565 = [((int(p[0]) >> 3) << 11) | ((int(p[1]) >> 2) << 5) | (int(p[2]) >> 3) for p in pal]
    lines = ["/* generated by tools/gen_splash_anim.py from assets/splash/kitty_CY.gif: do not edit */",
             f"#define SPLASH_ANIM_W {W}", f"#define SPLASH_ANIM_H {SPLASH_H}", f"#define SPLASH_ANIM_N {len(fr)}",
             f"#define SPLASH_ANIM_MS {MS}",
             "static const uint16_t SPLASH_ANIM_PAL[%d] = {%s};" % (len(rgb565), ", ".join(f"0x{c:04X}" for c in rgb565)),
             f"static const uint8_t SPLASH_ANIM[{len(blob)}] = {{"]
    for i in range(0, len(blob), 24):
        lines.append("    " + ", ".join(str(b) for b in blob[i:i + 24]) + ",")
    lines.append("};")
    path.write_text("\n".join(lines) + "\n")
    print(f"-> {path}")


if __name__ == "__main__":
    main()
