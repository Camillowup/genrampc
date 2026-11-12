#!/usr/bin/env python3
"""build_skin.py - writes CHOP TEST's Plugin Skins: three hand-made pages BROWSER, PADS and SETUP.

The SDK's gen_ui.py lays out pages of 16 controls. CHOP TEST needs rows and pads to tap, big buttons and texts the
plugin changes live. This script uses gen_ui's own building blocks (same JSON shapes, same writer with its duplicate
"version" keys). The knobs (MODUS, CHOPS, PITCH, VOLUME) and the LISTE slider reuse the component definitions gen_ui
builds from ui.json; the page gen_ui would make from ui.json itself is dropped. Images come from skin-extra/.

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
PADS = 16
WHITE = "ffffffff"
CENTRED = "horizontallyCentred verticallyCentred"
LEFT = "left verticallyCentred"


class Page:
    """Children of one tab, plus helpers. h = parameter name -> handle ("Parameter N")."""

    def __init__(self, h, style, defs):
        self.h, self.style, self.defs, self.c = h, style, defs, []

    def image(self, png, b, name=None):
        data = G.obj(("version", 2), ("imageType", "Regular"), ("colour", "0"), ("image", png))
        self.c.append(G.entry(name or "img %d" % len(self.c), "Image", data, b, "No", "Show", "Always"))

    def strip(self, png, frames, param, b):
        data = G.obj(("version", 2), ("imageType", "FilmStrip"), ("colour", "0"), ("image", png),
                     ("numFrames", frames), ("handleName", "Data"))
        self.c.append(G.entry("strip %d" % len(self.c), "Image", data, b, "No", "Show", "Always",
                              remap=[("Data", self.h[param])]))

    def value(self, param, b, height, colour=WHITE, just=LEFT):
        self.c.append(G.entry("value %d" % len(self.c), "Label", G.label_data("Value", colour, height, just), b,
                              "No", "Show", "Always", remap=[("Data", self.h[param])]))

    def text(self, text, b, height, colour=WHITE, just=LEFT):
        self.c.append(G.entry("text %d" % len(self.c), "Label", G.label_data("Text", colour, height, just, text), b,
                              "No", "Show", "Always"))

    def button(self, param, png, b, button_id=1, group=1, off_png=None):
        self.c.append(G.entry("button %d" % len(self.c), "Button",
                              G.button_data(png, off_png or png, button_id, group), b, "No", "Show", "Always",
                              remap=[("Data", self.h[param])]))

    def big_button(self, param, bg, clear, caption, b, height=34.0):
        self.image(bg, b)
        self.text(caption, b, height, WHITE, CENTRED)
        self.button(param, clear, b)

    def control(self, param, b):
        """A knob / slider slot (278 x 107) using the definition gen_ui made for this parameter."""
        self.c.append(G.entry(param, self.defs[param], G.obj(("version", 1), ("handleName", "Data")), b,
                              "Yes", "Hide", "Always", remap=[("Data", self.h[param])]))

    def tab(self, name, index):
        comp = G.component_definition([], self.c, self.style["background"])
        return G.obj(("version", 1), ("tabName", name), ("fnKeyIndex", index), ("fnKeySubIndex", 0),
                     ("componentDefinition", comp), ("qlinkBoundsData", list(G.QLINK_BOUNDS)))


def browser(p):
    muted = p.style["muted"]
    p.image("br_path.png", "20 6 1240 52")
    p.value("PFAD", "38 6 1210 52", 30.0)
    for k in range(ROWS):
        y = 66 + k * 52
        p.image("br_row.png", "20 %d 880 48" % y)
        p.value("ZEILE %d" % (k + 1), "38 %d 850 48" % y, 28.0)
        p.button("TIPP %d" % (k + 1), "br_clear_row.png", "20 %d 880 48" % y)
    p.big_button("ZURUECK", "br_red.png", "br_clear_big.png", "ZURÜCK", "920 66 340 110")
    p.big_button("LAUFWERKE", "br_purple.png", "br_clear_big.png", "LAUFWERKE", "920 186 340 110")
    p.big_button("SEITE ZURUECK", "br_grey.png", "br_clear_small.png", "< SEITE", "920 306 165 110")
    p.big_button("SEITE VOR", "br_grey.png", "br_clear_small.png", "SEITE >", "1095 306 165 110")
    p.value("LISTINFO", "920 424 340 40", 26.0, muted, CENTRED)
    p.control("LISTE", "951 466 278 107")
    p.text("Geladen:", "20 584 150 40", 26.0, muted)
    p.value("GELADEN", "170 584 1090 40", 28.0, p.style["accent"])


def pads(p):
    accent = p.style["accent"]
    p.image("br_path.png", "20 6 1240 52")
    p.value("GELADEN", "38 6 400 52", 28.0, accent)
    p.value("TONART", "450 6 200 52", 26.0)
    p.value("TEMPO", "660 6 360 52", 26.0)
    p.value("CHOPINFO", "1030 6 220 52", 26.0)
    for r in range(4):          # r = 0 is the top row: pads 13-16, like on the MPC
        for col in range(4):
            k = (3 - r) * 4 + col
            x, y = 20 + col * 150, 66 + r * 122
            p.strip("pad_strip.png", 2, "LICHT %d" % (k + 1), "%d %d 140 112" % (x, y))
            p.value("NUMMER %d" % (k + 1), "%d %d 116 44" % (x + 14, y + 6), 32.0)
            p.value("AKKORD %d" % (k + 1), "%d %d 116 44" % (x + 14, y + 58), 28.0)
            p.button("PAD %d" % (k + 1), "clear_pad.png", "%d %d 140 112" % (x, y))
    p.control("MODUS", "650 66 278 107")
    p.control("CHOPS", "960 66 278 107")
    p.control("PITCH", "650 183 278 107")
    p.control("VOLUME", "960 183 278 107")
    for i, name in enumerate("ABCD"):
        b = "%d 310 140 90" % (650 + i * 152)
        p.button("BANK", "bank_on.png", b, button_id=i, group=4, off_png="bank_off.png")
        p.text(name, b, 40.0, WHITE, CENTRED)
    p.big_button("CHOP SPEICHERN", "br_save.png", "clear_save.png", "CHOP SPEICHERN", "650 420 600 100")
    p.value("ZULETZT", "20 560 1240 44", 26.0)


def setup(p):
    muted = p.style["muted"]
    p.big_button("LERNEN", "br_learn.png", "clear_learn.png", "LERNEN", "20 20 400 140", 44.0)
    p.value("LERNINFO", "20 175 400 50", 28.0)
    p.text("LERNEN antippen, dann die", "20 240 400 36", 24.0, muted)
    p.text("Pads 1 bis 16 der Reihe nach", "20 276 400 36", 24.0, muted)
    p.text("druecken (Pad 1 = unten links).", "20 312 400 36", 24.0, muted)
    for i, (label, param) in enumerate((("Pad-Belegung", "BELEGUNG"), ("Laufwerke", "LAUFWERKSLISTE"),
                                        ("Schreiben", "SCHREIBEN"), ("CHOP LINK", "CHOP LINK"),
                                        ("Analyse", "ANALYSE"))):
        y = 20 + i * 70
        p.image("br_row.png", "450 %d 810 60" % y)
        p.text(label, "470 %d 220 60" % y, 26.0, muted)
        p.value(param, "690 %d 560 60" % y, 26.0)


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
    for i, (name, fn) in enumerate((("BROWSER", browser), ("PADS", pads), ("SETUP", setup))):
        pg = Page(h, style, defs)
        fn(pg)
        tabs.append(pg.tab(name, i))
    # BANK steps through A-D on the data wheel too
    for k, v in page_data:
        if k == "replacementStrings":
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
