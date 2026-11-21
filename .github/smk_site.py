#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""The SMK-37 installer pages (the CI's Pages step): web/make_site.py builds SLOOP's installer, whose text is the FM-1's.
This rewrites a built installer page (OUT_DIR/webapp/installer/index.html) for the SMK-37 and for one of the two builds:
its heading, its lead and tiles, and the FM-1's words in the messages (both languages); the FM-1 Transporter's
recovery becomes the SMK-37's own (its USB recovery screen). SLOOP's web/ is not changed.

  .github/smk_site.py OUT_DIR stems|mewcoder|harmoniser MODEL_NAME [VERSION]

(VERSION: the build's, e.g. 2.8.4-mewcoder-harmoniser-a8b1ae9: the editor's side bar names the build and shows it.)
"""
import base64
import html as htmlmod
import re
import shutil
import sys
from pathlib import Path

out, build, model = Path(sys.argv[1]), sys.argv[2], sys.argv[3]
version = sys.argv[4] if len(sys.argv) > 4 else ""
page = out / "webapp" / "installer" / "index.html"
html = page.read_text(encoding="utf-8")

HEAD = {
    "stems": (f"SLOOP for the {model}: Multichannel USB audio",
              "SLOOP 2.8.1 as a 6-channel USB audio interface: your DAW records each track and drum group on a channel "
              "of its own. Punch-in effects on every channel, the punch-in page on the pads, sequencer effects and DAW "
              "mode. Every engine, 153 sounds and the built-in samples.",
              [("6", "USB audio channels to the DAW"), ("27", "punch-in effects, on every channel"),
               ("16", "pads: the stem punch-in page"), ("DAW", "mode, with your own CC map")]),
    "mewcoder": (f"Mewcoder for the {model}",
                 "SLOOP 2.8.4 with your voice, or any sound you choose, in over USB audio on track 3, played dry or through a 20-channel vocoder "
                 "modelled on the Sennheiser VSM201, played from the keys and the sequencer. A lean build: the built-in "
                 "samples and some engines make room for it.",
                 [("20", "vocoder channels"), ("2", "inputs: voice, carrier, tracks"),
                  ("EXT", "your synth as the carrier"), ("HQ", "44.1 kHz vocoder")]),
    "harmoniser": (f"Mewcoder + Harmoniser for the {model}",
                   "Mewcoder now with its own harmoniser: your voice, or any sound you choose, in over USB audio, into up "
                   "to three harmony voices modelled on the Eventide H949, with a lane of it across every track, each "
                   "harmonised on its own channel. Mewcoder's 20-channel vocoder is all there. Early testing / "
                   "pre-release. A lean build: the built-in samples and some engines make room for it.",
                   [("3", "harmony voices, after the H949"), ("4", "lanes: every track harmonised"),
                    ("20", "vocoder channels"), ("2", "USB inputs, routed")]),
}
title, lead, tiles = HEAD[build]
COLS = ["blue", "green", "yellow", "orange"]


def sub1(pat, rep, s, flags=0, expand=False):
    s2, n = re.subn(pat, (lambda m: m.expand(rep)) if expand else (lambda m: rep), s, count=1, flags=flags)
    if n != 1:
        raise SystemExit(f"smk_site.py: not found: {pat[:60]}")
    return s2


# the dancing cats (every build's boot screen, smk37-devtools splash-spec.md): the owner's GIF, inlined, so the page and
# the editor's one file for the disk show them offline too
CATS = "data:image/gif;base64," + base64.b64encode((Path(__file__).parent / "pages" / "cats.gif").read_bytes()).decode()
CATS_IMG = f'<img class="cats" src="{CATS}" width="454" height="477" alt="Two dancing cats: the boot screen">'
html = sub1(r"<h1>.*?</h1>", f"<h1>{title}</h1>", html, re.S)
html = sub1(r"<title>SLOOP installer</title>", "<title>" + {"stems": "Multichannel", "mewcoder": "Mewcoder",
            "harmoniser": "Mewcoder + Harmoniser"}[build] + " installer</title>", html)
html = sub1(r'<p class="lead">.*?</p>', f'<p class="lead">{lead}</p>', html, re.S)
html = sub1(r'<div class="tiles">.*?</div>\s*</div>',
            '<div class="tiles">\n' + "".join(
                f'    <div class="tile" style="--c: var(--{c})"><b>{b}</b><span>{t}</span></div>\n'
                for (b, t), c in zip(tiles, COLS)) + "  </div>", html, re.S)
# the FM-1 Transporter link: the SMK-37 recovers by itself
html = sub1(r'\s*<a href="https://github.com/kurogedelic/FM-1-transporter"[^>]*>FM-1-transporter</a>', "", html)
html = sub1(r"M-VAVE and FM-1 are trademarks", "M-VAVE, SMK-37 and FM-1 are trademarks", html)
if build in ("mewcoder", "harmoniser"):   # no built-in samples in these builds
    html = sub1(r" Samples: Versilian Studios[^<]*?not CC0\)\.", "", html)

# the manual (a link beside the editor's) and the screens, under the editor links. The screens are the firmware's own
# 240 x 240 screens (drawn from its menu and page code), in .github/pages/shots/, copied next to the page
REPO = "https://github.com/MiloMeowzers/sloop-smk37"
MANUAL = {"stems": (f"{REPO}/blob/ci/SMK37.md", "The SMK-37 manual"),      # (Mewcoder and Mewcoder + Harmoniser share
          "mewcoder": (f"{REPO}/blob/mewcoder/VOCODER.md", "The vocoder manual"),   # the manuals on the harmoniser
          "harmoniser": (f"{REPO}/blob/harmoniser/HARMONISER.md", "The harmoniser manual")}[build]   # branch)
# the vocoder builds: the SMK-37 manual too (Selectable Punch In Effects, BEAT REPEAT, VARIATION, the fader banks)
MANUAL2 = None if build == "stems" else (f"{REPO}/blob/{build}/SMK37.md", "The SMK-37 manual")
SITE = "https://milomeowzers.github.io/sloop-smk37/" + {"stems": "", "mewcoder": "mewcoder/webapp/installer/",
                                                        "harmoniser": "harmoniser/webapp/installer/"}[build]
BNAME = {"stems": "Multichannel", "mewcoder": "Mewcoder", "harmoniser": "Mewcoder + Harmoniser"}[build]
# "2.8.4-mewcoder-harmoniser-a8b1ae9" -> "2.8.4  a8b1ae9" (the boot screen's version line)
VSHOW = f"{version.split('-')[0]}  {version.split('-')[-1]}" if "-" in version else version
EDTITLE = f"{BNAME} editor"
# the build's name and version (as the keyboard's boot screen shows them: the "+" and the version in magenta), at the
# top of the installer and of the editor's side bar, in place of SLOOP's logo
BRAND = ("<b>" + BNAME.upper().replace(" + ", " <i>+</i> ") + "</b>" +
         (f"<small>{htmlmod.escape(VSHOW)}</small>" if VSHOW else ""))
BRAND_CSS = (".smkb { display: flex; flex-direction: column; align-items: flex-start; gap: 4px; }"
             " .smkb b { font-size: 15px; font-weight: 600; letter-spacing: .06em; line-height: 1.25; }"
             " .smkb i { font-style: normal; color: #ff40ff; }"
             " .smkb small { font-size: 12px; color: #ff40ff; white-space: pre; }")
# the installer's top, above the line: the cats on the left, the build's name and version right-aligned beside them
# (the language switch under them); the line in the cats' colours (cyan, yellow, magenta, white), the install button
# and its progress in cyan
CYAN, YELLOW, MAGENTA = "#18d6ee", "#f6f610", "#ff40ff"
html = sub1(r'<div class="top"><div class="logo">.*?</div>(<button class="lang" id="lang"></button>)</div>',
            rf'<div class="top">{CATS_IMG}<div class="smkb hdr">{BRAND}\1</div></div>', html, re.S, expand=True)
html = sub1(r"</head>", f"<style>{BRAND_CSS}"
            " .top { align-items: flex-end; } .top .cats { width: 150px; height: auto; background: #000; border-radius: 8px; }"
            " .smkb.hdr { align-items: flex-end; text-align: right; } .smkb.hdr b { font-size: 18px; }"
            " .smkb.hdr .lang { margin-top: 6px; }"
            " @media (max-width: 520px) { .top .cats { width: 110px; } .smkb.hdr b { font-size: 15px; } }"
            f" .strip i:nth-child(1) {{ background: {CYAN}; }} .strip i:nth-child(2) {{ background: {YELLOW}; }}"
            f" .strip i:nth-child(3) {{ background: {MAGENTA}; }} .strip i:nth-child(4) {{ background: var(--fg); }}"
            f" button#go {{ background: {CYAN}; }} progress::-webkit-progress-value {{ background: {CYAN}; }}"
            f" progress::-moz-progress-bar {{ background: {CYAN}; }}"
            "</style>\n</head>", html)
# the package's line once it is checked: the build, not "SLOOP <version>"
html = sub1(r"say\(`SLOOP \$\{meta\.version\}`\);", f'say("{BNAME}  {VSHOW}".trim());', html)
SHOTS = {
    "stems": [("stems-punch", "The stem punch-in page", "FX held: REV playing on every stem"),
              ("stems-usb", "HOME > USB", "six stems: the drums in four groups"),
              ("stems-menu-punch", "HOME > PUNCH", "the punch-ins on the stems, seamless joins")],
    "mewcoder": [("voc-channels", "The vocoder page", "the 20 channels on the knobs and pads"),
                 ("voc-bridge", "Channels 8-14 and BRIDGE", "silence bridging, AUTOCONTROL"),
                 ("voc-setup", "HOME > SETUP", "voice on IN L, your synth as the carrier on IN R"),
                 ("voc-envelope", "HOME > ENVELOPE", "attack, release, BRIDGE MODE"),
                 ("punch-page2", "Selectable Punch In Effects", "the stems punched, BEAT REPEAT per track"),
                 ("stem-level", "HOME > STEMS", "STEM LEVEL: hotter stems for the DAW")],
    "harmoniser": [("harm-page", "The HARMONY page", "the PITCH RATIO readout, the interval pads"),
                   ("harm-faders", "FADER BANK F", "each track's MIX: dry to harmony"),
                   ("harm-load", "HOME > LANE RATE", "half-rate voices per lane"),
                   ("harm-poly", "HOME > POLY CAP", "a synth capped while the harmoniser runs"),
                   ("punch-page2", "Selectable Punch In Effects", "the stems punched, BEAT REPEAT per track"),
                   ("stem-level", "HOME > STEMS", "STEM LEVEL: hotter stems for the DAW"),
                   ("voc-channels", "The vocoder page", "the 20 channels on the knobs and pads"),
                   ("harm-voc-setup", "HOME > SETUP", "voice on IN L, your synth as the carrier on IN R")],
}[build]
shots_dir = page.parent / "shots"
shots_dir.mkdir(exist_ok=True)
for name, _, _ in SHOTS:
    shutil.copyfile(Path(__file__).parent / "pages" / "shots" / f"{name}.png", shots_dir / f"{name}.png")
html = sub1(r'(\s*<p><button id="app" class="ed" hidden data-t="appInstall"></button></p>)',
            f'\n  <p><a class="ed" href="{MANUAL[0]}" target="_blank" rel="noopener">{MANUAL[1]}</a></p>' +
            (f'\n  <p><a class="ed" href="{MANUAL2[0]}" target="_blank" rel="noopener">{MANUAL2[1]}</a></p>'
             if MANUAL2 else "") +
            r'\1' + '\n  <h2 class="shots-h">On the keyboard</h2>\n  <div class="shots">\n' + "".join(
                f'    <figure><img src="shots/{n}.png" width="240" height="240" alt="" loading="lazy">'
                f'<figcaption><b>{c}</b>{s}</figcaption></figure>\n' for n, c, s in SHOTS) + "  </div>", html, expand=True)
html = sub1(r"(\s*\[hidden\] \{ display: none !important; \})",
            r"\1" + "\n  .shots-h { font-size: 13px; font-weight: 600; letter-spacing: .08em; text-transform: uppercase;"
            " color: var(--dim); margin: 28px 0 10px; }\n"
            "  .shots { display: grid; grid-template-columns: repeat(2, 1fr); gap: 12px; }\n"
            "  .shots figure { margin: 0; }\n"
            "  .shots img { display: block; width: 100%; height: auto; image-rendering: pixelated; background: #000; }\n"
            "  .shots figcaption { font-size: 12px; color: var(--dim); margin-top: 6px; line-height: 1.35; }\n"
            "  .shots figcaption b { display: block; color: var(--fg); font-weight: 600; }", html, expand=True)

EN = {
    "intro": f"Connect the {model} by USB. (Works in Chrome or Edge.)",
    "direct": "Connect the USB cable directly to the computer.",
    "brick": "If an install fails and the keyboard no longer starts: after two failed starts it opens its USB recovery "
             "screen (or hold OCTAVE− while switching on). Then install again from here.",
    "beta": "Unofficial firmware, not affiliated with M-Vave, and it may have faults. The author accepts no responsibility "
            "if your keyboard is damaged or misbehaves because of it. Use it at your own risk.",
    "back": "To go back to M-Vave's firmware, use M-Vave's own updater, M-UPGRADE (link below). If it does not find the "
            "keyboard, close every other app and browser tab that uses MIDI.",
    "notfound": "Keyboard not found. Check the USB cable (a data cable), close other apps that use MIDI, and press "
                "Install again.",
    "model": f"The connected keyboard is not an {model}. Pick the page for your model.",
    "noloader": "The keyboard in update mode was not found. Replug the USB cable and press Install again (it stays in "
                "update mode until it is finished).",
    "noreturn": "The keyboard did not come back. Switch it off and on.",
    "mismatch": "Written, but the keyboard reports another version: ",
    "foreign": "The keyboard is in the update mode of another firmware. Finish that update with its own updater "
               "(M-UPGRADE), then press Install again: ",
    "editor": "Editor (after installing: sounds, samples, projects"
              + {"stems": "", "mewcoder": ", the vocoder", "harmoniser": ", the vocoder and the harmoniser"}[build]
              + ", from the computer)",
}
JA = {
    "intro": f"{model} を USB で接続してください。（Chrome か Edge で使えます）",
    "direct": "USB ケーブルはパソコンに直接つないでください。",
    "brick": "書き込みに失敗してキーボードが起動しなくなった場合: 起動に 2 回失敗すると USB の復旧画面が開きます"
             "（または OCTAVE− を押しながら電源を入れます）。そのあと、ここからもう一度インストールしてください。",
    "beta": "M-Vave とは関係のない非公式のファームウェアで、不具合がある場合があります。これを使ったことでキーボードが壊れたり"
            "不具合が起きたりしても、作者は一切の責任を負いません。自己責任でお使いください。",
    "back": "M-Vave のファームウェアに戻すには、M-Vave のアップデーター M-UPGRADE（下のリンク）を使ってください。"
            "キーボードが見つからない場合は、MIDI を使う他のアプリとブラウザのタブを閉じてください。",
    "notfound": "キーボードが見つかりません。USB ケーブル（データ通信用か）を確かめ、MIDI を使う他のアプリを閉じてから、"
                "もう一度押してください。",
    "model": f"接続されている機器は {model} ではありません。お使いのモデルのページを選んでください。",
    "noloader": "書き込みモードのキーボードが見つかりません。USB を挿し直してもう一度押してください（終わるまで書き込みモードのままです）。",
    "noreturn": "キーボードが戻ってきません。電源を入れ直してください。",
    "mismatch": "書き込みは終わりましたが、キーボードが別のバージョンを報告しています: ",
    "foreign": "キーボードは別のファームウェアの書き込みモードです。そのアップデーター（M-UPGRADE）で書き込みを終えてから、"
               "もう一度インストールしてください: ",
}


def strings(block, table):
    for k, v in table.items():
        v = v.replace("\\", "\\\\").replace('"', '\\"')
        block, n = re.subn(r'(\b' + k + r':\s*)"(?:[^"\\]|\\.)*"', lambda m: m.group(1) + '"' + v + '"', block)
        if not n:
            raise SystemExit(f"smk_site.py: string {k} not found")
    return block


# the language button and the "other firmware" message shared the key "other" (the message, later in each table, won:
# the button showed the whole message): the button gets a key of its own
html = sub1(r'\$\("lang"\)\.textContent = t\("other"\);', '$("lang").textContent = t("langName");', html)
html = sub1(r'install: "インストール", other: "English",', 'install: "インストール", langName: "English",', html)
html = sub1(r'install: "Install", other: "日本語",', 'install: "Install", langName: "日本語",', html)

i = html.index("\n  en: {")
html = strings(html[:i], JA) + strings(html[i:], EN)
page.write_text(html, encoding="utf-8")
print(f"smk_site.py: {page} ({build}, {model})")

# the icon (the owner's, 2026-11-20: the cat in the magenta ring; .github/pages/icon-*.png, from icon-source.png): in
# place of SLOOP's in webapp/ (the installer's and the editor's favicon, the app's icons), and the app's name the build's
PAGES = Path(__file__).parent / "pages"
for f in ("icon-192.png", "icon-512.png", "icon-maskable-512.png"):
    shutil.copy(PAGES / f, out / "webapp" / f)
mf = out / "webapp" / "sloop.webmanifest"
if mf.exists():
    import json
    m = json.loads(mf.read_text(encoding="utf-8"))
    m["name"], m["short_name"] = BNAME, BNAME
    m["description"] = f"{BNAME} for the {model}: the editor and the installer, also without internet"
    mf.write_text(json.dumps(m, indent=1) + "\n", encoding="utf-8")
ICON48 = "data:image/png;base64," + base64.b64encode((PAGES / "icon-48.png").read_bytes()).decode()

# the editor (both copies: online and the one file for the disk): the device it talks to is the SMK-37. The FM-1's name
# only ever appears in its words (help, messages, labels): the device is found by a lower-case pattern, never by it
KEEP = r"FM-1(?![-_.]| V15)"
for ed in (out / "webapp" / "editor" / "index.html", out / "webapp" / "editor" / "sloop-editor.html"):
    if not ed.exists():
        continue
    e = ed.read_text(encoding="utf-8")
    e = e.replace("M-VAVE and FM-1 are trademarks", "M-VAVE, SMK-37 and FM-1 are trademarks")
    e = sub1(r"<title>SLOOP Editor</title>", f"<title>{EDTITLE}</title>", e)   # (the build's name, before connecting)
    if ed.name == "sloop-editor.html":                 # (the one file for the disk has no site around it: its icon inlined)
        e = sub1(r"</title>", f'</title>\n<link rel="icon" href="{ICON48}">', e)
    e, n = re.subn(KEEP, "SMK-37", e)
    e = e.replace("M-VAVE, SMK-37 and SMK-37 are trademarks", "M-VAVE, SMK-37 and FM-1 are trademarks")
    # the one file for the disk linked SLOOP's FM-1 installer: this build's own instead
    e = e.replace('href="https://isod89.github.io/sloop-fm1/webapp/installer/"', f'href="{SITE}"')
    # the manual, beside the installer link under the connect button
    e = sub1(r'(<a class="dim" href="[^"]*"(?: target="_blank" rel="noopener")? data-t="installer"></a>)',
             rf'\1<a class="dim" href="{MANUAL[0]}" target="_blank" rel="noopener">Manual</a>' +
             (rf'<a class="dim" href="{MANUAL2[0]}" target="_blank" rel="noopener">SMK-37 manual</a>'
              if MANUAL2 else ""), e, expand=True)
    # the side bar's top: the build's name and its version, in place of SLOOP's logo and "editor"
    e = sub1(r'<a class="brand" href="#sound" aria-label="SLOOP">.*?</a>',
             f'<a class="brand smkb" href="#sound" aria-label="{htmlmod.escape(BNAME)}">{BRAND}</a>', e, re.S)
    # (the side bar's top tighter: the name is shorter than the logo was)
    e = sub1(r"</head>", f"<style>{BRAND_CSS}"
             " @media (min-width: 861px) { .side { padding-top: 12px; } .brand.smkb { margin-bottom: -8px; } }"
             "</style>\n</head>", e)
    # the vocoder line's editors name themselves on connecting ("SLOOP Editor: Mewcoder ..."): the build's name
    e = e.replace('"SLOOP Editor: Mewcoder + Harmoniser"', '"Mewcoder + Harmoniser editor"')
    e = e.replace('"SLOOP Editor: Mewcoder"', '"Mewcoder editor"').replace(': "SLOOP Editor";', f': "{EDTITLE}";')
    # SLOOP's installer link names this build's installer (it links there already)
    for a, b in (('installer: "SLOOP installer"', 'installer: "Installer"'),
                 ('installer: "SLOOP インストーラー"', 'installer: "インストーラー"')):
        if e.count(a) != 1:
            raise SystemExit(f"smk_site.py: not found once: {a}")
        e = e.replace(a, b)
    # the dancing cats in the device card, directly above the connect button, always: as tall as the window leaves
    # room for (190 px at 1080, 90 px at 800, none under ~710, so the button stays in view); hidden where the side bar becomes a
    # top bar
    e = sub1(r'<button id="connect"', f'{CATS_IMG}\n      <button id="connect"', e)
    e = sub1(r"</head>", "<style>.devcard .cats { height: clamp(0px, calc(100vh - 710px), 190px); width: auto;"
             " max-width: 100%; align-self: center; object-fit: contain; background: #000; border-radius: 8px; }"
             " @media (max-width: 860px) { .devcard .cats { display: none; } }"
             # the connect button in the cats' cyan, as the installer's install button (white under the pointer)
             f" button#connect.pri:not(:disabled) {{ background: {CYAN}; }}"
             " button#connect.pri:hover:not(:disabled) { background: var(--fg); }"
             # (the vocoder builds' second manual link: the side bar's links row wraps instead of running over)
             " .devcard .links { flex-wrap: wrap; justify-content: flex-start; column-gap: 12px; row-gap: 4px; }"
             "</style>\n</head>", e)
    ed.write_text(e, encoding="utf-8")
    print(f"smk_site.py: {ed}: {n} x FM-1 -> SMK-37")
