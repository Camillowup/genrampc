#!/usr/bin/env python3
"""build_skin.py - writes CHOP TEST's Plugin Skins: three hand-made pages BROWSER, PADS and SETUP.

The SDK's gen_ui.py lays out pages of 16 controls. CHOP TEST needs rows and pads to tap, big buttons, a waveform and
texts the plugin changes live. This script uses gen_ui's own building blocks (same JSON shapes, same writer with its
duplicate "version" keys). The knobs (MODUS, CHOPS, PITCH, VOLUME) and the LISTE slider reuse the component
definitions gen_ui builds from ui.json; the page gen_ui would make from ui.json itself is dropped.

What the MPC showed us: fixed texts in a skin are not drawn, texts from the plugin are. So every caption is a
parameter ("CAP <key>"). Images bound to a button are drawn at their native size; other images are scaled to their
bounds. Images come from skin-extra/ (tools/make_images.py draws them). The look follows GENRA CHOP in VCV Rack.

Usage (after sdk/tools/build.py has built the plugin):
    python chop-test/tools/build_skin.py "chop-test/CHOP TEST"
"""

import os
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.abspath(os.path.join(HERE, "..", "..", "sdk", "tools")))
import gen_ui as G  # noqa: E402

ROWS = 10
BARS = 64
BAR_FRAMES = 48

BEIGE = "ffd9d3c4"
TEXT = "ff2c2c2c"
MUTED = "ff736d62"
LCD_TEXT = "ff1f2a18"
WHITE = "ffffffff"
RED = "ffc8282d"
CENTRED = "horizontallyCentred verticallyCentred"
LEFT = "left verticallyCentred"


class Page:
    """Children of one tab, plus helpers. h = parameter name -> handle ("Parameter N")."""

    def __init__(self, h, defs):
        self.h, self.defs, self.c = h, defs, []

    def image(self, png, b):
        data = G.obj(("version", 2), ("imageType", "Regular"), ("colour", "0"), ("image", png))
        self.c.append(G.entry("img %d" % len(self.c), "Image", data, b, "No", "Show", "Always"))

    def strip(self, png, frames, param, b):
        data = G.obj(("version", 2), ("imageType", "FilmStrip"), ("colour", "0"), ("image", png),
                     ("numFrames", frames), ("handleName", "Data"))
        self.c.append(G.entry("strip %d" % len(self.c), "Image", data, b, "No", "Show", "Always",
                              remap=[("Data", self.h[param])]))

    def value(self, param, b, height, colour=TEXT, just=LEFT):
        self.c.append(G.entry("value %d" % len(self.c), "Label", G.label_data("Value", colour, height, just), b,
                              "No", "Show", "Always", remap=[("Data", self.h[param])]))

    def caption(self, key, b, height, colour=TEXT, just=LEFT):
        """Fixed texts in the skin are not drawn on the MPC; the plugin serves each caption as "CAP <key>"."""
        self.value("CAP " + key, b, height, colour, just)

    def button(self, param, png, b, button_id=1, group=1, off_png=None):
        self.c.append(G.entry("button %d" % len(self.c), "Button",
                              G.button_data(png, off_png or png, button_id, group), b, "No", "Show", "Always",
                              remap=[("Data", self.h[param])]))

    def flash_button(self, param, flash, strip_png, b, key=None, text_param=None, height=32.0, colour=WHITE):
        """A button: its image is a 2-frame film strip bound to the flash parameter (it lights up when tapped),
        a caption on top (a CAP key, or a text parameter), an invisible button over all of it."""
        x, y, w, hgt = b.split()
        self.strip(strip_png, 2, "BLITZ %d" % (flash + 1), b)
        if key:
            self.caption(key, b, height, colour, CENTRED)
        if text_param:
            self.value(text_param, b, height, colour, CENTRED)
        self.button(param, "clear_%sx%s.png" % (w, hgt), b)

    def control(self, param, b):
        """A knob / slider slot (278 x 107) using the definition gen_ui made for this parameter."""
        self.c.append(G.entry(param, self.defs[param], G.obj(("version", 1), ("handleName", "Data")), b,
                              "Yes", "Hide", "Always", remap=[("Data", self.h[param])]))

    def tab(self, name, index, qlink_bounds):
        comp = G.component_definition([], self.c, BEIGE)
        return G.obj(("version", 1), ("tabName", name), ("fnKeyIndex", index), ("fnKeySubIndex", 0),
                     ("componentDefinition", comp), ("qlinkBoundsData", list(qlink_bounds)))


# flash numbers (enum Flash in ChopTest.cpp)
F_BACK, F_DRIVES, F_PGUP, F_PGDN, F_SAVEHERE, F_SAVE, F_CHOKE, F_LEARN, F_REV, F_PDOWN, F_PUP, F_LOCK, F_ROW0 = range(13)


def browser(p):
    p.image("lcd.png", "20 6 1240 50")
    p.value("PFAD", "38 6 1210 50", 28.0, LCD_TEXT)
    for k in range(ROWS):
        y = 64 + k * 50
        b = "20 %d 880 46" % y
        p.strip("row.png", 2, "BLITZ %d" % (F_ROW0 + k + 1), b)
        p.value("ZEILE %d" % (k + 1), "38 %d 850 46" % y, 26.0)
        p.button("TIPP %d" % (k + 1), "clear_880x46.png", b)
    p.flash_button("ZURUECK", F_BACK, "btn_dark_340x80.png", "920 64 340 80", key="back")
    p.flash_button("LAUFWERKE", F_DRIVES, "btn_dark_340x80.png", "920 152 340 80", key="drives")
    p.flash_button("SEITE ZURUECK", F_PGUP, "btn_dark_165x80.png", "920 240 165 80", key="pgup")
    p.flash_button("SEITE VOR", F_PGDN, "btn_dark_165x80.png", "1095 240 165 80", key="pgdn")
    p.flash_button("SPEICHERORT HIER", F_SAVEHERE, "btn_yellow_340x70.png", "920 328 340 70", key="savehere",
                   height=26.0, colour=TEXT)
    p.value("LISTINFO", "920 404 340 32", 22.0, TEXT, CENTRED)
    p.image("panel_dark.png", "920 438 340 112")
    p.control("LISTE", "951 440 278 107")
    p.caption("loaded", "20 572 150 44", 26.0, MUTED)
    p.value("GELADEN", "170 572 1090 44", 28.0, RED)


def pads(p):
    # LCD bar
    p.image("lcd.png", "20 4 1240 46")
    p.value("GELADEN", "38 4 400 46", 24.0, LCD_TEXT)
    p.value("TONART", "450 4 200 46", 24.0, LCD_TEXT)
    p.value("TEMPO", "660 4 360 46", 24.0, LCD_TEXT)
    p.value("CHOPINFO", "1030 4 220 46", 24.0, LCD_TEXT)
    # the waveform: 64 bars, one film strip each (height x playing x border)
    p.image("panel_dark.png", "20 54 1240 88")
    for b in range(BARS):
        p.strip("wave_strip.png", BAR_FRAMES, "WELLE %d" % (b + 1), "%d 56 19 84" % (32 + b * 19))
    # the pads, 1-4 at the bottom like on the MPC
    for r in range(4):
        for col in range(4):
            k = (3 - r) * 4 + col
            x, y = 20 + col * 150, 150 + r * 108
            b = "%d %d 140 100" % (x, y)
            p.strip("pad_strip.png", 2, "LICHT %d" % (k + 1), b)
            p.value("NUMMER %d" % (k + 1), "%d %d 116 32" % (x + 12, y + 4), 28.0)
            p.value("AKKORD %d" % (k + 1), "%d %d 116 30" % (x + 12, y + 36), 24.0)
            p.value("MARKEN %d" % (k + 1), "%d %d 116 26" % (x + 12, y + 68), 20.0, RED)
            p.button("PAD %d" % (k + 1), "clear_140x100.png", b)
    p.value("ZULETZT", "20 584 600 40", 24.0)
    # knobs on a dark section, like the panels of GENRA CHOP
    p.image("panel_dark.png", "640 150 620 236")
    p.control("MODUS", "650 160 278 107")
    p.control("CHOPS", "960 160 278 107")
    p.control("PITCH", "650 270 278 107")
    p.control("VOLUME", "960 270 278 107")
    for i, name in enumerate("ABCD"):
        b = "%d 396 140 60" % (650 + i * 152)
        p.button("BANK", "bank_on.png", b, button_id=i, group=4, off_png="bank_off.png")
        p.caption("bank" + name, b, 34.0, WHITE, CENTRED)
    p.flash_button("CHOP SPEICHERN", F_SAVE, "btn_yellow_295x60.png", "650 466 295 60", key="save", height=26.0,
                   colour=TEXT)
    p.flash_button("CHOKE", F_CHOKE, "btn_dark_295x60.png", "955 466 295 60", text_param="CHOKETEXT", height=26.0)
    # edit row: the last played chop
    p.value("EDITTEXT", "640 534 236 44", 20.0)
    for i, (param, flash, key) in enumerate((("ED REV", F_REV, "rev"), ("ED PITCH AB", F_PDOWN, "pdown"),
                                             ("ED PITCH AUF", F_PUP, "pup"), ("ED LOCK", F_LOCK, "lock"))):
        p.flash_button(param, flash, "btn_dark_90x44.png", "%d 534 90 44" % (880 + i * 95), key=key, height=18.0)


def setup(p):
    p.flash_button("LERNEN", F_LEARN, "btn_red_400x130.png", "20 20 400 130", key="learn", height=44.0)
    p.value("LERNINFO", "20 165 400 50", 28.0)
    p.caption("help1", "20 230 400 36", 24.0, MUTED)
    p.caption("help2", "20 266 400 36", 24.0, MUTED)
    p.caption("help3", "20 302 400 36", 24.0, MUTED)
    for i, (key, param) in enumerate((("r1", "BELEGUNG"), ("r2", "LAUFWERKSLISTE"), ("r3", "SCHREIBEN"),
                                      ("r4", "CHOP LINK"), ("r5", "ANALYSE"), ("r6", "SPEICHERORT"))):
        y = 20 + i * 70
        p.image("setup_row.png", "450 %d 810 60" % y)
        p.caption(key, "470 %d 220 60" % y, 24.0, MUTED)
        p.value(param, "690 %d 560 60" % y, 24.0)


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    project = sys.argv[1]
    build = os.path.join(project, "build")
    outs = [os.path.join(build, d, "Plugin Skins") for d in sorted(os.listdir(build))
            if os.path.isdir(os.path.join(build, d, "Plugin Skins"))]
    if not outs:
        print("build_skin: no built plugin folder under %s (run sdk/tools/build.py first)" % build)
        return 1
    out_dir = outs[0]

    params = G.load_params(os.path.join(build, "params.json"))
    h = {q.name: q.handle for q in params}
    table = G.ParamTable(params)
    ui_pages, style, knob_image, frames, kit = G.load_ui(os.path.join(project, "ui.json"), table)
    kit_dir, kit_assets = G.kit_dir_for(kit, build, out_dir) if kit else (None, {})
    tui = G.build_tui(ui_pages, params, style, knob_image, frames, kit_assets)
    defs = {c.param.name: c.definition for pg in ui_pages for c in pg.controls if c.param is not None}

    page_data = dict(tui)["pageData"]
    tabs = dict(page_data)["tabs"]
    del tabs[:]
    # the Q-Link highlight: around LISTE (browser), around the four knobs (pads), none (setup)
    pages = (("BROWSER", browser, ["930 440 320 108"]), ("PADS", pads, ["645 155 610 226"]), ("SETUP", setup, []))
    for i, (name, fn, ql) in enumerate(pages):
        pg = Page(h, defs)
        fn(pg)
        tabs.append(pg.tab(name, i, ql))
    for k, v in page_data:
        if k == "replacementStrings":   # BANK steps through A-D on the data wheel too
            v.append(G.obj(("key", h["BANK"]), ("value", ["A", "B", "C", "D"])))

    def qmap(assign):
        return G.Obj(("Q-Link %d" % q, assign.get(q, -1)) for q in range(1, 17))
    idx = {q.name: q.index for q in params}
    screen = [G.obj(("Tab", 1), ("SubTab", 1), ("Bank Direction", "Column"), ("Q-Links", qmap({13: idx["LISTE"]}))),
              G.obj(("Tab", 2), ("SubTab", 1), ("Bank Direction", "Column"),
                    ("Q-Links", qmap({13: idx["MODUS"], 14: idx["CHOPS"], 15: idx["PITCH"], 16: idx["VOLUME"]}))),
              G.obj(("Tab", 3), ("SubTab", 1), ("Bank Direction", "Column"), ("Q-Links", qmap({})))]
    program = qmap({1: idx["MODUS"], 2: idx["CHOPS"], 3: idx["PITCH"], 4: idx["VOLUME"]})
    qlinks = G.obj(("version", 4), ("info", G.obj(("version", 1), ("type", "CompleteDescription"))),
                   ("Screen Mode Q-Links", G.obj(("version", 4), ("map", screen))),
                   ("Program Mode Q-Links", program))

    # images: the kit's and ours; then drop whatever no page uses
    used = G.referenced_images(tui, set())
    if kit_dir and os.path.abspath(kit_dir) != os.path.abspath(out_dir):
        for img in sorted(used):
            if os.path.isfile(os.path.join(kit_dir, img)):
                shutil.copy2(os.path.join(kit_dir, img), os.path.join(out_dir, img))
    extra = os.path.join(project, "skin-extra")
    for f in sorted(os.listdir(extra)):
        if f in used:
            shutil.copy2(os.path.join(extra, f), os.path.join(out_dir, f))
    for f in os.listdir(out_dir):
        if f.lower().endswith(".png") and f not in used:
            os.remove(os.path.join(out_dir, f))
    missing = sorted(i for i in used if not os.path.isfile(os.path.join(out_dir, i)))
    if missing:
        print("build_skin: missing images: %s" % ", ".join(missing))
        return 1

    with open(os.path.join(out_dir, "TUI.json"), "w", encoding="utf-8", newline="\n") as f:
        f.write(G.dump_json(tui) + "\n")
    with open(os.path.join(out_dir, "Q-Links.json"), "w", encoding="utf-8", newline="\n") as f:
        f.write(G.dump_json(qlinks) + "\n")
    print("build_skin: wrote BROWSER, PADS, SETUP to %s" % out_dir)
    return 0


if __name__ == "__main__":
    sys.exit(main())
