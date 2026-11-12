#!/usr/bin/env python3
"""build_skin.py - writes GENRA CHOP's Plugin Skins: the pages BROWSER, CHOP and SETUP, and draws every image.

Look: the retro MPC (beige panel, grey and red keys, dark displays with cream text). The MPC draws images at their
native size (nothing is scaled), so every box is drawn here at exactly the size the layout asks for; the PNGs are
written with the standard library only (zlib). Fixed texts in a skin are not drawn on the MPC: every caption is a
read-only parameter the plugin serves ("CAP <key>"). Film strips (display columns, chop slots, steps, lane cells,
lamps) get their frame from the plugin as the parameter's value.

Usage (after sdk/tools/build.py has built the plugin):
    python genra-chop/tools/build_skin.py "genra-chop/GENRA CHOP"
"""

import math
import os
import struct
import sys
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.abspath(os.path.join(HERE, "..", "..", "sdk", "tools")))
import gen_ui as G  # noqa: E402

# ------------------------------------------------------------------ colours (RGB) and label colours (ARGB)
DARK = (0x2B, 0x2D, 0x2A)
CREAM = (0xE8, 0xDF, 0xC4)
RED = (0xC0, 0x32, 0x2B)
RED_LIT = (0xE0, 0x50, 0x3F)
GREY = (0xB9, 0xB2, 0xA0)
KEY_OFF = (0x7E, 0x7C, 0x75)
LANE = (0x5A, 0x5C, 0x58)
KNOB = (0x3A, 0x3A, 0x37)
SEL_BG = (0x47, 0x42, 0x3B)

L_DARK = "ff2a2926"
L_MUTED = "ff5a5850"
L_CREAM = "ffe8dfc4"
L_CREAM_MUTED = "ffb9b2a0"
L_WHITE = "ffffffff"
L_RED = "ffc0322b"
CENTRED = "horizontallyCentred verticallyCentred"
LEFT = "left verticallyCentred"
RIGHT = "right verticallyCentred"

ROWS, PADS, COLS, LANES = 10, 16, 124, 4
COL_W, COL_H, COL_FRAMES, COL_LEVELS = 10, 136, 48, 16


# ------------------------------------------------------------------ PNG drawing (RGBA rows of bytearrays)
class Img:
    def __init__(self, w, h):
        self.w, self.h = w, h
        self.px = [bytearray(w * 4) for _ in range(h)]

    def put(self, x, y, rgb, a=255):
        if 0 <= x < self.w and 0 <= y < self.h:
            r = self.px[y]
            r[x * 4:x * 4 + 4] = bytes((rgb[0], rgb[1], rgb[2], a))

    def rect(self, x0, y0, w, h, rgb, radius=0):
        for y in range(max(0, y0), min(self.h, y0 + h)):
            for x in range(max(0, x0), min(self.w, x0 + w)):
                if radius:
                    dx = max(x0 + radius - x - 0.5, 0, x - (x0 + w - radius) + 0.5)
                    dy = max(y0 + radius - y - 0.5, 0, y - (y0 + h - radius) + 0.5)
                    if dx * dx + dy * dy > radius * radius:
                        continue
                self.put(x, y, rgb)

    def outline(self, x0, y0, w, h, rgb, t, radius=0):
        for y in range(y0, y0 + h):
            for x in range(x0, x0 + w):
                inner = x0 + t <= x < x0 + w - t and y0 + t <= y < y0 + h - t
                if inner:
                    continue
                if radius:
                    dx = max(x0 + radius - x - 0.5, 0, x - (x0 + w - radius) + 0.5)
                    dy = max(y0 + radius - y - 0.5, 0, y - (y0 + h - radius) + 0.5)
                    if dx * dx + dy * dy > radius * radius:
                        continue
                self.put(x, y, rgb)

    def disc(self, cx, cy, r, rgb):
        for y in range(int(cy - r - 1), int(cy + r + 2)):
            for x in range(int(cx - r - 1), int(cx + r + 2)):
                if (x + 0.5 - cx) ** 2 + (y + 0.5 - cy) ** 2 <= r * r:
                    self.put(x, y, rgb)

    def line(self, x0, y0, x1, y1, rgb, width):
        n = int(max(abs(x1 - x0), abs(y1 - y0)) * 2) + 1
        for i in range(n + 1):
            t = i / n
            self.disc(x0 + (x1 - x0) * t, y0 + (y1 - y0) * t, width / 2.0, rgb)

    def png(self):
        raw = b"".join(b"\x00" + bytes(r) for r in self.px)
        def chunk(t, d):
            return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xffffffff)
        return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", self.w, self.h, 8, 6, 0, 0, 0))
                + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def strip(frames):
    """Stack frames (Img of the same size) into one vertical film strip."""
    out = Img(frames[0].w, frames[0].h * len(frames))
    for i, f in enumerate(frames):
        for y in range(f.h):
            out.px[i * f.h + y] = f.px[y]
    return out


class Images:
    """Draws each image once, on demand, named after what it is."""

    def __init__(self, out_dir):
        self.out_dir, self.done = out_dir, set()

    def _write(self, name, make):
        if name not in self.done:
            with open(os.path.join(self.out_dir, name), "wb") as f:
                f.write(make().png())
            self.done.add(name)
        return name

    def box(self, rgb, w, h, radius=6):
        def make():
            i = Img(w, h)
            i.rect(0, 0, w, h, rgb, min(radius, w // 2, h // 2))
            return i
        return self._write("box_%02x%02x%02x_%dx%d.png" % (rgb + (w, h)), make)

    def clear(self, w, h):
        return self._write("clear_%dx%d.png" % (w, h), lambda: Img(w, h))

    def columns(self):
        def frame(state, level):
            i = Img(COL_W, COL_H)
            i.rect(0, 0, COL_W, COL_H, SEL_BG if state == 2 else DARK)
            bh = max(2, int(round((level + 1) / COL_LEVELS * (COL_H - 16))))
            i.rect(1, (COL_H - bh) // 2, 7, bh, (255, 255, 255) if state == 2 else CREAM, 1)
            if state == 1:
                i.rect(0, 0, 2, COL_H, RED_LIT)
            return i
        return self._write("col_strip.png", lambda: strip([frame(s, l) for s in range(3) for l in range(COL_LEVELS)]))

    def slots(self):
        def frame(state):
            i = Img(75, 52)
            i.rect(0, 0, 75, 52, RED if state == 2 else DARK, 5)
            if state == 1:
                i.outline(0, 0, 75, 52, CREAM, 3, 5)
            return i
        return self._write("slot_strip.png", lambda: strip([frame(s) for s in range(3)]))

    def steps(self):
        def frame(on):
            i = Img(72, 12)
            i.rect(0, 0, 72, 12, RED if on else GREY, 3)
            return i
        return self._write("step_strip.png", lambda: strip([frame(s) for s in range(2)]))

    def cells(self):
        def frame(f):
            i = Img(26, 36)
            note, now = f & 1, f >= 2
            if note:
                i.rect(2, 4, 22, 22, CREAM, 3)
            else:
                i.rect(10, 13, 6, 6, (0x6E, 0x70, 0x6B), 1)
            if now:
                i.rect(0, 31, 26, 5, RED_LIT, 2)
            return i
        return self._write("cell_strip.png", lambda: strip([frame(f) for f in range(4)]))

    def lamps(self):
        def frame(on):
            i = Img(16, 16)
            i.disc(8, 8, 7, RED_LIT if on else (0x5A, 0x58, 0x50))
            return i
        return self._write("lamp_strip.png", lambda: strip([frame(f) for f in range(2)]))

    def knob(self):
        def frame(k):
            i = Img(64, 64)
            i.disc(32, 32, 30, KNOB)
            a = math.radians(-135 + 270 * k / 126.0)
            i.line(32, 32, 32 + 22 * math.sin(a), 32 - 22 * math.cos(a), CREAM, 4)
            return i
        return self._write("knob64.png", lambda: strip([frame(k) for k in range(127)]))


# ------------------------------------------------------------------ the pages
class Page:
    """Children of one tab. h: parameter name -> handle ("Parameter N")."""

    def __init__(self, h, style, defs, img):
        self.h, self.style, self.defs, self.img, self.c = h, style, defs, img, []

    def image(self, png, b):
        data = G.obj(("version", 2), ("imageType", "Regular"), ("colour", "0"), ("image", png))
        self.c.append(G.entry("img %d" % len(self.c), "Image", data, b, "No", "Show", "Always"))

    def film(self, png, frames, param, b):
        data = G.obj(("version", 2), ("imageType", "FilmStrip"), ("colour", "0"), ("image", png),
                     ("numFrames", frames), ("handleName", "Data"))
        self.c.append(G.entry("strip %d" % len(self.c), "Image", data, b, "No", "Show", "Always",
                              remap=[("Data", self.h[param])]))

    def value(self, param, b, height, colour, just=LEFT):
        self.c.append(G.entry("value %d" % len(self.c), "Label", G.label_data("Value", colour, height, just), b,
                              "No", "Show", "Always", remap=[("Data", self.h[param])]))

    def name(self, param, b, height, colour, just=LEFT):
        self.c.append(G.entry("name %d" % len(self.c), "Label", G.label_data("Name", colour, height, just), b,
                              "No", "Show", "Always", remap=[("Data", self.h[param])]))

    def caption(self, key, b, height, colour, just=LEFT):
        self.value("CAP " + key, b, height, colour, just)

    def button(self, param, b, button_id=1, group=1, on_png=None, off_png=None):
        w, h = [int(v) for v in b.split()[2:]]
        clear = self.img.clear(w, h)
        self.c.append(G.entry("button %d" % len(self.c), "Button",
                              G.button_data(on_png or clear, off_png or clear, button_id, group), b, "No", "Show",
                              "Always", remap=[("Data", self.h[param])]))

    def key(self, param, rgb, cap, b, height=24.0, colour=L_WHITE):
        """A key: a coloured box, its caption, a transparent button on top."""
        x, y, w, h = [int(v) for v in b.split()]
        self.image(self.img.box(rgb, w, h), b)
        self.caption(cap, b, height, colour, CENTRED)
        self.button(param, b)

    def radio(self, param, caps, xs, y, w, h, height=22.0):
        on, off = self.img.box(RED, w, h), self.img.box(KEY_OFF, w, h)
        for i, (cap, x) in enumerate(zip(caps, xs)):
            b = "%d %d %d %d" % (x, y, w, h)
            self.button(param, b, button_id=i, group=len(caps), on_png=on, off_png=off)
            self.caption(cap, b, height, L_WHITE, CENTRED)

    def dark(self, b):
        x, y, w, h = [int(v) for v in b.split()]
        self.image(self.img.box(DARK, w, h), b)

    def control(self, param, b):
        self.c.append(G.entry(param, self.defs[param], G.obj(("version", 1), ("handleName", "Data")), b,
                              "Yes", "Hide", "Always", remap=[("Data", self.h[param])]))

    def knob(self, param, x, y):
        self.c.append(G.entry("knob %s" % param, "Knob", G.knob_filmstrip_data(self.img.knob(), 127),
                              "%d %d 64 64" % (x, y), "Yes", "Hide", "Always", remap=[("Data", self.h[param])]))
        self.name(param, "%d %d 92 28" % (x + 70, y + 2), 21.0, L_DARK)
        self.value(param, "%d %d 92 28" % (x + 70, y + 32), 20.0, L_MUTED)

    def tab(self, name, index, qlink_bounds):
        comp = G.component_definition([], self.c, self.style["background"])
        return G.obj(("version", 1), ("tabName", name), ("fnKeyIndex", index), ("fnKeySubIndex", 0),
                     ("componentDefinition", comp), ("qlinkBoundsData", list(qlink_bounds)))


def slot_x(i):
    return 20 + int(round(i * 77.5))


def chop_page(p):
    img = p.img
    # header
    p.caption("genra", "20 8 120 48", 34.0, L_DARK)
    p.caption("chop", "136 8 110 48", 34.0, L_RED)
    p.dark("250 9 330 46")
    p.value("GELADEN", "262 9 310 46", 23.0, L_CREAM)
    p.dark("590 9 190 46")
    p.value("KOPF", "590 9 190 46", 22.0, L_CREAM, CENTRED)
    p.value("LINKTEXT", "790 9 110 46", 22.0, L_DARK)
    p.radio("ANSICHT", ("sample", "loop", "chopv"), (906, 1026, 1146), 9, 114, 46, 21.0)
    # the display: one film strip per column
    cols = img.columns()
    for i in range(COLS):
        p.film(cols, COL_FRAMES, "SPALTE %d" % (i + 1), "%d 64 %d %d" % (20 + i * COL_W, COL_W, COL_H))
    # the chop strip: the 16 chops of the bank
    slots = img.slots()
    for i in range(PADS):
        x = slot_x(i)
        p.film(slots, 3, "SLOTLICHT %d" % (i + 1), "%d 206 75 52" % x)
        p.value("SLOTTEXT %d" % (i + 1), "%d 206 71 30" % (x + 4), 19.0, L_CREAM)
        p.value("SLOTLAENGE %d" % (i + 1), "%d 233 69 22" % (x + 6), 17.0, L_CREAM_MUTED)
        p.button("SLOT %d" % (i + 1), "%d 206 75 52" % x)
    # the 16 steps of the bar that plays
    steps = img.steps()
    for i in range(PADS):
        p.film(steps, 2, "SCHRITT %d" % (i + 1), "%d 266 72 12" % slot_x(i))
    # the lanes
    p.dark("20 288 560 332")
    lamps, cells = img.lamps(), img.cells()
    for k, cap in enumerate(("drums", "bass", "mel", "pads")):
        y = 292 + k * 78
        p.film(lamps, 2, "LAMPE %d" % (k + 1), "32 %d 16 16" % (y + 6))
        p.caption(cap, "56 %d 160 28" % y, 20.0, L_CREAM)
        p.value("SPURTEXT %d" % (k + 1), "220 %d 250 28" % y, 18.0, L_CREAM_MUTED, RIGHT)
        p.image(img.box(LANE, 444, 40, 4), "30 %d 444 40" % (y + 30))
        for j in range(PADS):
            p.film(cells, 4, "ZELLE %d %d" % (k + 1, j + 1), "%d %d 26 36" % (32 + int(round(j * 27.5)), y + 32))
        p.key("NEW %s" % ("DRUMS", "BASS", "MELODIE", "PADS")[k], RED, "new", "484 %d 86 54" % (y + 13), 22.0)
    p.value("TAKT", "32 596 300 22", 18.0, L_CREAM_MUTED)
    # STYLE and NEW ALL
    p.caption("style", "600 286 120 24", 18.0, L_MUTED)
    p.dark("600 312 360 52")
    p.value("STYLE", "614 312 340 52", 24.0, L_CREAM)
    p.key("STYLE ZURUECK", GREY, "prev", "968 312 46 52", 26.0, L_DARK)
    p.key("STYLE VOR", GREY, "next", "1020 312 46 52", 26.0, L_DARK)
    p.key("NEW ALL", RED, "newall", "1076 288 184 76", 30.0)
    # MODUS, CHOPS, BANK
    p.caption("modus", "600 370 120 22", 16.0, L_MUTED)
    p.caption("chops", "826 370 120 22", 16.0, L_MUTED)
    p.caption("bank", "1006 370 120 22", 16.0, L_MUTED)
    p.radio("MODUS", ("transient", "equal"), (600, 708), 394, 104, 46, 20.0)
    p.key("CHOPS MINUS", GREY, "minus", "826 394 44 46", 26.0, L_DARK)
    p.dark("874 394 68 46")
    p.value("CHOPZAHL", "874 394 68 46", 22.0, L_CREAM, CENTRED)
    p.key("CHOPS PLUS", GREY, "plus", "946 394 44 46", 26.0, L_DARK)
    p.radio("BANK", ("bankA", "bankB", "bankC", "bankD"), (1006, 1070, 1134, 1198), 394, 58, 46, 22.0)
    # UNDO, SAVE WAV, CHOKE, the selected chop
    p.key("UNDO", GREY, "undo", "600 450 150 46", 21.0, L_DARK)
    p.key("SAVE WAV", GREY, "savewav", "758 450 150 46", 21.0, L_DARK)
    p.dark("916 450 150 46")
    p.value("CHOKETEXT", "916 450 150 46", 21.0, L_CREAM, CENTRED)
    p.button("CHOKE", "916 450 150 46")
    p.value("AUSWAHL 1", "1078 450 184 24", 19.0, L_DARK)
    p.value("AUSWAHL 2", "1078 474 184 22", 17.0, L_MUTED)
    # the four knobs of the selected chop (one Q-Link group)
    for i, param in enumerate(("PITCH", "DECAY", "FILTER", "LEVEL")):
        p.knob(param, 600 + i * 165, 508)
    p.value("STATUS", "600 586 660 30", 19.0, L_DARK)


def browser(p):
    p.dark("20 6 1240 52")
    p.value("PFAD", "38 6 1210 52", 30.0, L_CREAM)
    for k in range(ROWS):
        y = 66 + k * 52
        p.dark("20 %d 880 48" % y)
        p.value("ZEILE %d" % (k + 1), "38 %d 850 48" % y, 28.0, L_CREAM)
        p.button("TIPP %d" % (k + 1), "20 %d 880 48" % y)
    p.key("ZURUECK", RED, "back", "920 66 340 90", 34.0)
    p.key("LAUFWERKE", KEY_OFF, "drives", "920 164 340 90", 34.0)
    p.key("SEITE ZURUECK", GREY, "pgup", "920 262 165 90", 30.0, L_DARK)
    p.key("SEITE VOR", GREY, "pgdn", "1095 262 165 90", 30.0, L_DARK)
    p.key("SPEICHERORT HIER", KEY_OFF, "savehere", "920 360 340 70", 26.0)
    p.value("LISTINFO", "920 434 340 36", 24.0, L_MUTED, CENTRED)
    p.control("LISTE", "951 472 278 107")
    p.caption("loaded", "20 584 150 40", 26.0, L_MUTED)
    p.value("GELADEN", "170 584 1090 40", 28.0, L_RED)


def setup(p):
    p.key("LERNEN", RED, "learn", "20 20 400 140", 44.0)
    p.value("LERNINFO", "20 175 400 50", 28.0, L_DARK)
    for i in range(3):
        p.caption("help%d" % (i + 1), "20 %d 400 36" % (240 + i * 36), 24.0, L_MUTED)
    rows = (("r1", "BELEGUNG"), ("r2", "LAUFWERKSLISTE"), ("r3", "SCHREIBEN"), ("r4", "LINKINFO"),
            ("r5", "ANALYSE"), ("r6", "SPEICHERORT"))
    for i, (key, param) in enumerate(rows):
        y = 20 + i * 70
        p.dark("450 %d 810 60" % y)
        p.caption(key, "470 %d 220 60" % y, 26.0, L_CREAM_MUTED)
        p.value(param, "690 %d 560 60" % y, 26.0, L_CREAM)


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
    for f in os.listdir(out_dir):
        if f.lower().endswith(".png"):
            os.remove(os.path.join(out_dir, f))

    params = G.load_params(os.path.join(build, "params.json"))
    h = {q.name: q.handle for q in params}
    table = G.ParamTable(params)
    ui_pages, style, knob_image, frames, kit = G.load_ui(os.path.join(project, "ui.json"), table)
    tui = G.build_tui(ui_pages, params, style, knob_image, frames, {})
    defs = {c.param.name: c.definition for pg in ui_pages for c in pg.controls if c.param is not None}

    page_data = dict(tui)["pageData"]
    tabs = dict(page_data)["tabs"]
    del tabs[:]
    img = Images(out_dir)
    pages = (("BROWSER", browser, ["941 464 298 123"]), ("CHOP", chop_page, ["596 502 664 76"]), ("SETUP", setup, []))
    for i, (name, fn, ql) in enumerate(pages):
        pg = Page(h, style, defs, img)
        fn(pg)
        tabs.append(pg.tab(name, i, ql))

    # the choice parameters step through their names on the data wheel
    styles = [q for q in params if q.name == "STYLE"]
    for k, v in page_data:
        if k == "replacementStrings":
            v.append(G.obj(("key", h["BANK"]), ("value", ["A", "B", "C", "D"])))
            v.append(G.obj(("key", h["ANSICHT"]), ("value", ["Sample", "Loop", "Chop"])))
            v.append(G.obj(("key", h["MODUS"]), ("value", ["Transient", "Equal"])))
            if styles and getattr(styles[0], "choices", None):
                v.append(G.obj(("key", h["STYLE"]), ("value", list(styles[0].choices))))

    def qmap(assign):
        return G.Obj(("Q-Link %d" % q, assign.get(q, -1)) for q in range(1, 17))
    idx = {q.name: q.index for q in params}
    screen = [G.obj(("Tab", 1), ("SubTab", 1), ("Bank Direction", "Row"), ("Q-Links", qmap({13: idx["LISTE"]}))),
              G.obj(("Tab", 2), ("SubTab", 1), ("Bank Direction", "Row"),
                    ("Q-Links", qmap({1: idx["PITCH"], 2: idx["DECAY"], 3: idx["FILTER"], 4: idx["LEVEL"],
                                      5: idx["STYLE"], 6: idx["CHOPS"], 7: idx["VOLUME"]}))),
              G.obj(("Tab", 3), ("SubTab", 1), ("Bank Direction", "Row"), ("Q-Links", qmap({})))]
    program = qmap({1: idx["PITCH"], 2: idx["DECAY"], 3: idx["FILTER"], 4: idx["LEVEL"], 5: idx["VOLUME"]})
    qlinks = G.obj(("version", 4), ("info", G.obj(("version", 1), ("type", "CompleteDescription"))),
                   ("Screen Mode Q-Links", G.obj(("version", 4), ("map", screen))),
                   ("Program Mode Q-Links", program))

    used = G.referenced_images(tui, set())
    missing = sorted(i for i in used if not os.path.isfile(os.path.join(out_dir, i)))
    if missing:
        print("build_skin: missing images: %s" % ", ".join(missing))
        return 1
    with open(os.path.join(out_dir, "TUI.json"), "w", encoding="utf-8", newline="\n") as f:
        f.write(G.dump_json(tui) + "\n")
    with open(os.path.join(out_dir, "Q-Links.json"), "w", encoding="utf-8", newline="\n") as f:
        f.write(G.dump_json(qlinks) + "\n")
    print("build_skin: wrote BROWSER, CHOP, SETUP (%d images) to %s" % (len(img.done), out_dir))
    return 0


if __name__ == "__main__":
    sys.exit(main())
