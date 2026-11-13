#!/usr/bin/env python3
"""build_skin.py - writes the Plugin Skins of GENRA CHOP, GENRA BASS, SCHLAG, GENRA MELODY and GENRA PADS and draws
every image.

Look: the retro MPC (keys, dark fields, LCDs like the MPC 2000); every plugin has the colours of a limited-edition MPC
(THEMES below). The MPC draws images at their native size (nothing is scaled), so every box is drawn here at
exactly the size the layout asks for; the PNGs are written with the standard library only (zlib). Fixed texts in a
skin are not drawn on the MPC: every caption is a read-only parameter the plugin serves ("CAP <key>"). Film strips
(display columns, slots, steps, cells, lamps, flashes) get their frame from the plugin as the parameter's value.

Usage (after sdk/tools/build.py has built the plugin):
    python genra-chop/tools/build_skin.py "genra-chop/GENRA CHOP"     (or "GENRA BASS", "SCHLAG")
"""

import json
import math
import os
import struct
import sys
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.abspath(os.path.join(HERE, "..", "..", "sdk", "tools")))
import gen_ui as G  # noqa: E402

DARK = (0x2B, 0x2D, 0x2A)
CREAM = (0xE8, 0xDF, 0xC4)
RED = (0xC0, 0x32, 0x2B)
RED_LIT = (0xE0, 0x50, 0x3F)
GREY = (0xB9, 0xB2, 0xA0)
KEY_OFF = (0x7E, 0x7C, 0x75)
PAD = (0x8C, 0x8B, 0x85)
LANE = (0x5A, 0x5C, 0x58)
KNOB = (0x3A, 0x3A, 0x37)
LCD = (0xB9, 0xC4, 0x6A)      # MPC 2000 green
LCD_SEL = (0xA3, 0xB0, 0x54)
LCD_DIM = (0x9A, 0xA6, 0x54)
LCD_INK = (0x26, 0x30, 0x1A)
DEAD = (0x4A, 0x54, 0x20)     # a dying LCD
DEAD_INK = (0x2C, 0x34, 0x12)
DEAD_DIM = (0x3F, 0x48, 0x19)
BLOB = (0x1C, 0x22, 0x09)

L_DARK = "ff2a2926"
L_MUTED = "ff5a5850"
L_CREAM = "ffe8dfc4"
L_CREAM_MUTED = "ffb9b2a0"
L_PAD = "fff4efe2"
L_WHITE = "ffffffff"
L_RED = "ffc0322b"
L_LCD = "ff26301a"
L_DEAD = "ff39421a"
L_KEYINK = "ff2a2926"   # text on the grey keys
SEL = RED               # a lit preset row

# Every GENRA plugin has its own colours, after a limited edition of the MPC (see the README).
# A theme overrides the names above; the drawing code reads them when it runs.
GREY_LCD = dict(LCD=(0xB4, 0xB7, 0xB0), LCD_SEL=(0xA4, 0xA7, 0xA0), LCD_DIM=(0x9C, 0x9F, 0x98),
                LCD_INK=(0x22, 0x24, 0x22), L_LCD="ff222422",
                DEAD=(0x6A, 0x6C, 0x66), DEAD_INK=(0x2E, 0x30, 0x2E), DEAD_DIM=(0x5A, 0x5C, 0x56),
                BLOB=(0x1A, 0x1B, 0x1A), L_DEAD="ff2e302e")
THEMES = {
    # GENRA CHOP: retro cream (MPC60 / MPC Live II Retro), green MPC 2000 LCD - the defaults above
    "GENRA CHOP": {},
    # GENRA BASS: white (MPC2500 LE)
    "GENRA BASS": dict(GREY_LCD, DARK=(0x1E, 0x1E, 0x1E), CREAM=(0xEE, 0xEE, 0xEA), GREY=(0xCF, 0xCF, 0xCA),
                       KEY_OFF=(0x8A, 0x8A, 0x86), PAD=(0x8A, 0x8A, 0x86), KNOB=(0x34, 0x34, 0x34),
                       L_DARK="ff1e1e1e", L_MUTED="ff555552", L_CREAM="ffeeeeea", L_CREAM_MUTED="ffa8a8a4",
                       L_KEYINK="ff1e1e1e"),
    # SCHLAG: black (MPC3000 LE)
    "SCHLAG": dict(GREY_LCD, DARK=(0x33, 0x33, 0x33), CREAM=(0xE8, 0xE8, 0xE4), GREY=(0x8C, 0x8C, 0x88),
                   KEY_OFF=(0x55, 0x55, 0x52), PAD=(0x6A, 0x6A, 0x66), LANE=(0x44, 0x44, 0x44),
                   KNOB=(0x48, 0x48, 0x48), RED=(0xD3, 0x2F, 0x2F), RED_LIT=(0xE8, 0x4A, 0x40),
                   L_DARK="ffe8e8e4", L_MUTED="ffa8a8a4", L_CREAM="ffe8e8e4", L_CREAM_MUTED="ff9a9a96",
                   L_PAD="fff0f0ec", L_RED="ffd32f2f", L_KEYINK="ff1c1c1c"),
    # GENRA MELODY: blue (MPC1000 / MPC One G2), grey display
    "GENRA MELODY": dict(GREY_LCD, DARK=(0x1D, 0x2B, 0x44), CREAM=(0xEE, 0xF2, 0xF7), GREY=(0xBF, 0xC3, 0xC7),
                         KEY_OFF=(0x7D, 0x97, 0xB8), PAD=(0x7D, 0x97, 0xB8), KNOB=(0x2C, 0x3B, 0x57),
                         L_DARK="fff2f4f7", L_MUTED="ffdce6f2", L_CREAM="ffeef2f7", L_CREAM_MUTED="ffa9bcd6",
                         L_KEYINK="ff1d2b44"),
    # GENRA PADS: red (MPC2000XL SE), black keys, grey display
    "GENRA PADS": dict(GREY_LCD, DARK=(0x1C, 0x1C, 0x1C), CREAM=(0xF0, 0xEC, 0xE6), GREY=(0x8C, 0x8C, 0x88),
                       KEY_OFF=(0x8C, 0x8C, 0x88), PAD=(0x8C, 0x8C, 0x88), KNOB=(0x3A, 0x3A, 0x3A),
                       RED=(0x1C, 0x1C, 0x1C), RED_LIT=(0xE0, 0x50, 0x3F), SEL=(0xE0, 0x50, 0x3F),
                       L_DARK="fff5ece8", L_MUTED="fff0d8d2", L_CREAM="fff0ece6", L_CREAM_MUTED="ffb5b0aa",
                       L_RED="ff1c1c1c", L_KEYINK="ff1c1c1c"),
}


def apply_theme(plugin):
    t = THEMES.get(plugin, {})
    if "RED" in t and "SEL" not in t:
        t = dict(t, SEL=t["RED"])
    globals().update(t)


CENTRED = "horizontallyCentred verticallyCentred"
LEFT = "left verticallyCentred"
RIGHT = "right verticallyCentred"
VOICE_NAMES = ["KICK", "SNARE", "CLAP", "RIM", "CL HAT", "OP HAT", "TOM", "PERC"]


def lighter(c, k=40):
    return tuple(min(255, v + k) for v in c)


# ------------------------------------------------------------------ PNG drawing
class Img:
    def __init__(self, w, h):
        self.w, self.h = w, h
        self.px = [bytearray(w * 4) for _ in range(h)]

    def put(self, x, y, rgb, a=255):
        if 0 <= x < self.w and 0 <= y < self.h:
            self.px[y][x * 4:x * 4 + 4] = bytes((rgb[0], rgb[1], rgb[2], a))

    @staticmethod
    def _inside(x, y, x0, y0, w, h, r):
        if not r:
            return True
        dx = max(x0 + r - x - 0.5, 0, x - (x0 + w - r) + 0.5)
        dy = max(y0 + r - y - 0.5, 0, y - (y0 + h - r) + 0.5)
        return dx * dx + dy * dy <= r * r

    def rect(self, x0, y0, w, h, rgb, radius=0):
        for y in range(max(0, y0), min(self.h, y0 + h)):
            for x in range(max(0, x0), min(self.w, x0 + w)):
                if self._inside(x, y, x0, y0, w, h, radius):
                    self.put(x, y, rgb)

    def outline(self, x0, y0, w, h, rgb, t, radius=0):
        for y in range(y0, y0 + h):
            for x in range(x0, x0 + w):
                if x0 + t <= x < x0 + w - t and y0 + t <= y < y0 + h - t:
                    continue
                if self._inside(x, y, x0, y0, w, h, radius):
                    self.put(x, y, rgb)

    def disc(self, cx, cy, r, rgb):
        for y in range(int(cy - r - 1), int(cy + r + 2)):
            for x in range(int(cx - r - 1), int(cx + r + 2)):
                if (x + 0.5 - cx) ** 2 + (y + 0.5 - cy) ** 2 <= r * r:
                    self.put(x, y, rgb)

    def ellipse(self, cx, cy, rx, ry, rgb, clip=None):
        for y in range(int(cy - ry - 1), int(cy + ry + 2)):
            for x in range(int(cx - rx - 1), int(cx + rx + 2)):
                if ((x + 0.5 - cx) / rx) ** 2 + ((y + 0.5 - cy) / ry) ** 2 <= 1.0:
                    if clip is None or clip(x, y):
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
    out = Img(frames[0].w, frames[0].h * len(frames))
    for i, f in enumerate(frames):
        for y in range(f.h):
            out.px[i * f.h + y] = f.px[y]
    return out


def dying(img, w, h, seed):
    """The damage of a dying LCD over a box: a dark bubble and a dead line."""
    cx = w * (0.72 if seed % 2 else 0.28)
    inside = lambda x, y: 0 <= x < w and 0 <= y < h and img.px[y][x * 4 + 3] > 0  # noqa: E731
    img.ellipse(cx, h * 0.55, max(6, w * 0.3), max(5, h * 0.75), BLOB, inside)
    img.ellipse(cx + 3, h * 0.5, max(3, w * 0.14), max(3, h * 0.4), (0x0F, 0x13, 0x05), inside)
    if seed % 3 == 0:
        x = int(w * 0.4)
        for y in range(h):
            if inside(x, y):
                img.put(x, y, (0x14, 0x19, 0x07))
    else:
        y = int(h * 0.35)
        for x in range(w):
            if inside(x, y):
                img.put(x, y, (0x14, 0x19, 0x07))


class Images:
    def __init__(self, out_dir):
        self.out_dir, self.done = out_dir, set()

    def _write(self, name, make):
        if name not in self.done:
            with open(os.path.join(self.out_dir, name), "wb") as f:
                f.write(make().png())
            self.done.add(name)
        return name

    def box(self, rgb, w, h, radius=6, dead=0):
        def make():
            i = Img(w, h)
            i.rect(0, 0, w, h, rgb, min(radius, w // 2, h // 2))
            if dead:
                dying(i, w, h, dead)
            return i
        return self._write("box_%02x%02x%02x_%dx%d%s.png" % (rgb + (w, h, "_d%d" % dead if dead else "")), make)

    def state(self, lit, n, w, h, radius=6):
        """A switch's state as an n-frame strip: frame `lit` red (on), the others grey (off)."""
        def make():
            fr = []
            for f in range(n):
                i = Img(w, h)
                i.rect(0, 0, w, h, RED if f == lit else KEY_OFF, min(radius, w // 2, h // 2))
                fr.append(i)
            return strip(fr)
        return self._write("state_%d_%d_%02x%02x%02x_%02x%02x%02x_%dx%d.png" % ((lit, n) + RED + KEY_OFF + (w, h)), make)

    def clear(self, w, h):
        return self._write("clear_%dx%d.png" % (w, h), lambda: Img(w, h))

    def two(self, a, b, w, h, radius=6):
        def make():
            fr = []
            for c in (a, b):
                i = Img(w, h)
                i.rect(0, 0, w, h, c, min(radius, w // 2, h // 2))
                fr.append(i)
            return strip(fr)
        return self._write("two_%02x%02x%02x_%02x%02x%02x_%dx%d.png" % (a + b + (w, h)), make)

    def columns(self, w=10, h=136):
        def frame(state, level):
            i = Img(w, h)
            i.rect(0, 0, w, h, LCD_SEL if state == 2 else LCD)
            bh = max(2, int(round((level + 1) / 16.0 * (h - 16))))
            i.rect(1, (h - bh) // 2, w - 3, bh, LCD_INK, 1)
            if state == 1:
                i.rect(0, 0, 3, h, RED_LIT)
            return i
        return self._write("col_strip.png", lambda: strip([frame(s, l) for s in range(3) for l in range(16)]))

    def slots(self):
        def frame(state):
            i = Img(75, 52)
            i.rect(0, 0, 75, 52, RED if state == 2 else PAD, 5)
            if state == 1:
                i.outline(0, 0, 75, 52, CREAM, 3, 5)
            return i
        return self._write("slot_strip.png", lambda: strip([frame(s) for s in range(3)]))

    def cells(self):
        def frame(f):
            i = Img(26, 36)
            if f & 1:
                i.rect(2, 4, 22, 22, CREAM, 3)
            else:
                i.rect(10, 13, 6, 6, (0x6E, 0x70, 0x6B), 1)
            if f >= 2:
                i.rect(0, 31, 26, 5, RED_LIT, 2)
            return i
        return self._write("cell_strip.png", lambda: strip([frame(f) for f in range(4)]))

    def lamps(self):
        def frame(on):
            i = Img(16, 16)
            i.disc(8, 8, 7, RED_LIT if on else (0x5A, 0x58, 0x50))
            return i
        return self._write("lamp_strip.png", lambda: strip([frame(f) for f in range(2)]))

    def knob(self, size=64):
        def frame(k):
            i = Img(size, size)
            c = size / 2.0
            i.disc(c, c, c - 2, KNOB)
            a = math.radians(-135 + 270 * k / 126.0)
            i.line(c, c, c + (c - 10) * math.sin(a), c - (c - 10) * math.cos(a), CREAM, max(3, size // 16))
            return i
        return self._write("knob%d.png" % size, lambda: strip([frame(k) for k in range(127)]))

    def ring(self, size=72):
        """The endless RANDOM knob: a ring of 24 dots, lit up to the value; no pointer."""
        def frame(k):
            i = Img(size, size)
            c = size / 2.0
            i.disc(c, c, c - 1, KNOB)
            lit = int(round(k / 126.0 * 24))
            for d in range(24):
                a = d / 24.0 * 2 * math.pi
                i.disc(c + (c - 8) * math.sin(a), c - (c - 8) * math.cos(a), 2.6, RED_LIT if d < lit else (0x5A, 0x5C, 0x58))
            i.disc(c, c, c * 0.55, (0x32, 0x32, 0x30))
            return i
        return self._write("ring%d.png" % size, lambda: strip([frame(k) for k in range(127)]))

    def line_cols(self, w=19, h=176):
        """GENRA BASS: one column per step: 0 empty, 1..16 a note at that height, +17 the step that plays."""
        def frame(f):
            now, lvl = f >= 17, f % 17
            i = Img(w, h)
            i.rect(0, 0, w, h, LCD_SEL if now else LCD)
            if lvl:
                y = h - 12 - int((lvl - 1) / 15.0 * (h - 36))
                i.rect(1, y - 6, w - 2, 10, LCD_INK, 2)
            if now:
                i.rect(w // 2 - 1, 0, 3, h, RED_LIT)
            return i
        return self._write("line_strip.png", lambda: strip([frame(f) for f in range(34)]))

    def chord_cols(self, w=19, h=172):
        """GENRA PADS: one column per step: frame = voices * 4 + 2 (a chord starts here) + 1 (the step that plays)."""
        def frame(f):
            n, start, now = f // 4, (f >> 1) & 1, f & 1
            i = Img(w, h)
            i.rect(0, 0, w, h, LCD_SEL if now else LCD)
            for v in range(n):
                y = h - 22 - v * 38
                x0 = 4 if start else 0
                i.rect(x0, y, w - x0, 11, LCD_INK, 2 if start else 0)
            if now:
                i.rect(w // 2 - 1, 0, 3, h, RED_LIT)
            return i
        return self._write("chord_strip.png", lambda: strip([frame(f) for f in range(20)]))

    def pad_cells(self, w, h):
        """GENRA PADS, PADS mode: a pad of the chord map; frame 0 = nothing (the display), 1 = a pad, 2 = it plays."""
        def frame(f):
            i = Img(w, h)  # frame 0 stays transparent: the chords of AUTO show through
            if f:
                i.rect(0, 0, w, h, RED_LIT if f == 2 else (0x22, 0x24, 0x22), 5)
            return i
        return self._write("padcell_strip.png", lambda: strip([frame(f) for f in range(3)]))

    def drum_cells(self, dead):
        def frame(f):
            i = Img(6, 18)
            if f == 0:
                i.rect(0, 0, 6, 18, DEAD_DIM if dead else LCD_DIM, 1)
            else:
                i.rect(0, 0, 6, 18, RED if f == 2 else (DEAD_INK if dead else LCD_INK), 1)
            return i
        return self._write("dcell%s_strip.png" % ("_d" if dead else ""), lambda: strip([frame(f) for f in range(3)]))


# ------------------------------------------------------------------ pages
class Page:
    flashes = {}  # key parameter -> its flash strip parameter

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
        """A key: a box (a 2-frame flash strip if the key flashes), its caption, a transparent button on top."""
        x, y, w, h = [int(v) for v in b.split()]
        flash = self.flashes.get(param)
        if flash:
            self.film(self.img.two(rgb, lighter(rgb, 55), w, h), 2, flash, b)
        else:
            self.image(self.img.box(rgb, w, h), b)
        self.caption(cap, b, height, colour, CENTRED)
        self.button(param, b)

    def state_key(self, param, state, cap, b, height=20.0):
        x, y, w, h = [int(v) for v in b.split()]
        self.film(self.img.two(KEY_OFF, RED, w, h), 2, state, b)
        self.caption(cap, b, height, L_WHITE, CENTRED)
        self.button(param, b)

    # Switches: the state is a film strip under the caption, the button on top is transparent. A caption lying on top
    # of a button swallows the tap on the MPC (the switches did not react), so the button must be the last layer.
    def toggle(self, param, cap, b, height=20.0):
        x, y, w, h = [int(v) for v in b.split()]
        self.film(self.img.state(1, 2, w, h), 2, param, b)
        self.caption(cap, b, height, L_WHITE, CENTRED)
        self.button(param, b, 1, 1)

    def radio(self, param, caps, xs, y, w, h, height=22.0):
        n = len(caps)
        for i, (cap, x) in enumerate(zip(caps, xs)):
            b = "%d %d %d %d" % (x, y, w, h)
            self.film(self.img.state(i, n, w, h), n, param, b)
            self.caption(cap, b, height, L_WHITE, CENTRED)
            self.button(param, b, button_id=i, group=n)

    def box(self, rgb, b, radius=6, dead=0):
        x, y, w, h = [int(v) for v in b.split()]
        self.image(self.img.box(rgb, w, h, radius, dead), b)

    def dark(self, b):
        self.box(DARK, b)

    def lcd(self, b, dead=0):
        self.box(DEAD if dead else LCD, b, 5, dead)

    def control(self, param, b):
        self.c.append(G.entry(param, self.defs[param], G.obj(("version", 1), ("handleName", "Data")), b,
                              "Yes", "Hide", "Always", remap=[("Data", self.h[param])]))

    def knob_only(self, param, b, png):
        self.c.append(G.entry("knob %d" % len(self.c), "Knob", G.knob_filmstrip_data(png, 127), b, "Yes", "Hide",
                              "Always", remap=[("Data", self.h[param])]))

    def knob(self, param, x, y, size=64, name_colour=L_DARK, value_colour=L_MUTED, below=False):
        self.knob_only(param, "%d %d %d %d" % (x, y, size, size), self.img.knob(size))
        if below:
            self.name(param, "%d %d %d 24" % (x - 40, y + size + 2, size + 80), 17.0, name_colour, CENTRED)
            self.value(param, "%d %d %d 22" % (x - 40, y + size + 24, size + 80), 15.0, value_colour, CENTRED)
        else:
            self.name(param, "%d %d 110 28" % (x + size + 8, y + 2), 21.0, name_colour)
            self.value(param, "%d %d 110 28" % (x + size + 8, y + 32), 19.0, value_colour)

    def tab(self, name, index, qlink_bounds):
        comp = G.component_definition([], self.c, self.style["background"])
        return G.obj(("version", 1), ("tabName", name), ("fnKeyIndex", index), ("fnKeySubIndex", 0),
                     ("componentDefinition", comp), ("qlinkBoundsData", list(qlink_bounds)))


def slot_x(i):
    return 20 + int(round(i * 77.5))


def print_page(p, title_x, has_dir_buttons):
    """The PRINT page, the same in every plugin."""
    p.caption("printtitle", "%d 8 200 48" % title_x, 28.0, L_MUTED)
    p.key("PRINT", RED, "print", "20 70 400 200", 54.0)
    p.value("PRINTINFO", "20 280 400 30", 17.0, L_MUTED, CENTRED)
    p.caption("after", "20 326 300 26", 18.0, L_DARK)
    p.radio("AUTO PADS", ("auto", "padsmode"), (20, 220), 356, 196, 60, 22.0)
    p.caption("autoinfo", "20 430 400 26", 17.0, L_MUTED)
    p.caption("padsinfo", "20 456 400 26", 17.0, L_MUTED)
    p.lcd("450 70 810 46")
    p.value("PRINTORDNER", "462 70 790 46", 21.0, L_LCD)
    for r in range(7):
        y = 124 + r * 52
        p.lcd("450 %d 810 46" % y)
        p.value("PRINTZEILE %d" % (r + 1), "462 %d 790 46" % y, 19.0, L_LCD)
    if has_dir_buttons:
        p.key("PRINT HERE", KEY_OFF, "printhere", "450 494 400 60", 20.0)
        p.key("PRINT STANDARD", KEY_OFF, "printstd", "860 494 400 60", 20.0)
    p.lcd("20 586 1240 32")
    p.value("PRINTSTATUS", "32 586 1220 32", 17.0, L_LCD)


# ------------------------------------------------------------------ GENRA CHOP
CHOP_FLASH = ["NEW ALL", "NEW DRUMS", "NEW BASS", "NEW MELODY", "NEW PADS", "UNDO", "SAVE WAV", "CHOPS MINUS",
              "CHOPS PLUS", "STYLE BACK", "STYLE NEXT", "PRINT", "LEARN", "BACK", "DRIVES", "PAGE BACK",
              "PAGE NEXT", "SAVE HERE", "PRINT HERE", "PRINT STANDARD"]


class ChopPage(Page):
    flashes = {k: "BLITZ %d" % (i + 1) for i, k in enumerate(CHOP_FLASH)}


def chop_header(p):
    p.caption("genra", "20 8 120 48", 34.0, L_DARK)
    p.caption("chop", "136 8 110 48", 34.0, L_RED)


def chop_page(p):
    img = p.img
    chop_header(p)
    p.dark("250 9 330 46")
    p.value("GELADEN", "262 9 310 46", 22.0, L_CREAM)
    p.dark("590 9 310 46")
    p.value("KOPF", "590 9 310 46", 20.0, L_CREAM, CENTRED)
    p.radio("VIEW", ("sample", "loop", "chopv"), (910, 1028, 1146), 9, 112, 46, 19.0)
    cols = img.columns()
    for i in range(124):
        p.film(cols, 48, "SPALTE %d" % (i + 1), "%d 64 10 136" % (20 + i * 10))
    slots = img.slots()
    for i in range(16):
        x = slot_x(i)
        p.film(slots, 3, "SLOTLICHT %d" % (i + 1), "%d 206 75 52" % x)
        p.value("SLOTTEXT %d" % (i + 1), "%d 206 71 30" % (x + 4), 18.0, L_PAD)
        p.value("SLOTLAENGE %d" % (i + 1), "%d 233 71 22" % (x + 4), 15.0, "ffe3ded0")
        p.button("SLOT %d" % (i + 1), "%d 206 75 52" % x)
    steps = img.two(GREY, RED, 72, 12, 3)
    for i in range(16):
        p.film(steps, 2, "SCHRITT %d" % (i + 1), "%d 266 72 12" % slot_x(i))
    p.dark("20 288 560 332")
    lamps, cells = img.lamps(), img.cells()
    for k, cap in enumerate(("drums", "bass", "mel", "pads")):
        y = 292 + k * 78
        p.film(lamps, 2, "LAMPE %d" % (k + 1), "32 %d 16 16" % (y + 6))
        p.caption(cap, "56 %d 160 28" % y, 19.0, L_CREAM)
        p.value("SPURTEXT %d" % (k + 1), "220 %d 250 28" % y, 17.0, L_CREAM_MUTED, RIGHT)
        p.box(LANE, "30 %d 444 40" % (y + 30), 4)
        for j in range(16):
            p.film(cells, 4, "ZELLE %d %d" % (k + 1, j + 1), "%d %d 26 36" % (32 + int(round(j * 27.5)), y + 32))
        p.key("NEW %s" % ("DRUMS", "BASS", "MELODY", "PADS")[k], RED, "new", "484 %d 86 54" % (y + 13), 21.0)
    p.value("TAKT", "32 596 300 22", 17.0, L_CREAM_MUTED)
    p.caption("style", "600 286 120 24", 16.0, L_MUTED)
    p.lcd("600 312 260 52")
    p.value("STYLE", "612 312 244 52", 21.0, L_LCD)
    p.key("STYLE BACK", GREY, "prev", "868 312 46 52", 26.0, L_KEYINK)
    p.key("STYLE NEXT", GREY, "next", "920 312 46 52", 26.0, L_KEYINK)
    p.key("NEW ALL", RED, "newall", "976 288 284 76", 30.0)
    p.caption("modus", "600 370 120 22", 15.0, L_MUTED)
    p.caption("chops", "826 370 120 22", 15.0, L_MUTED)
    p.caption("bank", "1006 370 120 22", 15.0, L_MUTED)
    p.radio("MODE", ("transient", "equal"), (600, 708), 394, 104, 46, 18.0)
    p.key("CHOPS MINUS", GREY, "minus", "826 394 44 46", 26.0, L_KEYINK)
    p.dark("874 394 68 46")
    p.value("CHOPZAHL", "874 394 68 46", 22.0, L_CREAM, CENTRED)
    p.key("CHOPS PLUS", GREY, "plus", "946 394 44 46", 26.0, L_KEYINK)
    p.radio("BANK", ("bankA", "bankB", "bankC", "bankD"), (1006, 1070, 1134, 1198), 394, 58, 46, 22.0)
    p.key("UNDO", GREY, "undo", "600 450 84 46", 18.0, L_KEYINK)
    p.radio("AUTO PADS", ("auto", "padsmode"), (690, 772), 450, 78, 46, 18.0)
    p.toggle("CHOKE", "choke", "856 450 96 46", 18.0)
    p.state_key("REV", "REVLICHT", "rev", "958 450 146 46", 18.0)
    p.state_key("LOCK", "LOCKLICHT", "lock", "1110 450 150 46", 18.0)
    for i, param in enumerate(("PITCH", "DECAY", "FILTER", "LEVEL")):
        p.knob(param, 600 + i * 165, 508)
    p.lcd("600 586 520 32")
    p.value("STATUS", "612 586 500 32", 17.0, L_LCD)
    p.key("SAVE WAV", GREY, "savewav", "1128 586 132 32", 16.0, L_KEYINK)


def chop_browser(p):
    p.dark("20 6 1240 52")
    p.value("PFAD", "38 6 1210 52", 28.0, L_CREAM)
    for k in range(10):
        y = 66 + k * 52
        p.dark("20 %d 880 48" % y)
        p.value("ZEILE %d" % (k + 1), "38 %d 850 48" % y, 26.0, L_CREAM)
        p.button("TIPP %d" % (k + 1), "20 %d 880 48" % y)
    p.key("BACK", RED, "back", "920 66 340 90", 32.0)
    p.key("DRIVES", KEY_OFF, "drives", "920 164 340 90", 32.0)
    p.key("PAGE BACK", GREY, "pgup", "920 262 165 90", 28.0, L_KEYINK)
    p.key("PAGE NEXT", GREY, "pgdn", "1095 262 165 90", 28.0, L_KEYINK)
    p.key("SAVE HERE", KEY_OFF, "savehere", "920 360 340 70", 24.0)
    p.value("LISTINFO", "920 434 340 36", 22.0, L_MUTED, CENTRED)
    p.control("LIST", "951 472 278 107")
    p.caption("loaded", "20 584 150 40", 24.0, L_MUTED)
    p.value("GELADEN", "170 584 1090 40", 26.0, L_RED)


def chop_print(p):
    chop_header(p)
    print_page(p, 252, True)


def chop_setup(p):
    p.key("LEARN", RED, "learn", "20 20 400 140", 42.0)
    p.value("LERNINFO", "20 175 400 50", 26.0, L_DARK)
    for i in range(4):
        p.caption("help%d" % (i + 1), "20 %d 400 34" % (238 + i * 34), 22.0, L_MUTED)
    rows = (("r1", "BELEGUNG"), ("r2", "LAUFWERKSLISTE"), ("r3", "SCHREIBEN"), ("r4", "LINKINFO"),
            ("r5", "ANALYSE"), ("r6", "SPEICHERORT"))
    for i, (key, param) in enumerate(rows):
        y = 20 + i * 70
        p.dark("450 %d 810 60" % y)
        p.caption(key, "470 %d 220 60" % y, 24.0, L_CREAM_MUTED)
        p.value(param, "690 %d 560 60" % y, 24.0, L_CREAM)


# ------------------------------------------------------------------ GENRA BASS, GENRA MELODY, GENRA PADS
class TonePage(Page):
    flashes = {"NEW": "BLITZ 1", "UNDO": "BLITZ 2", "PRINT": "BLITZ 3"}


NAME_W = {"GENRA BASS": 110, "GENRA MELODY": 150, "GENRA PADS": 110}
TONE_SOUND = {
    "GENRA BASS": (("WAVE", "SUB", "DETUNE", "NOISE"), ("CUTOFF", "RESO", "ENV", "DRIVE"),
                   ("ATTACK", "DECAY", "SUSTAIN", "RELEASE")),
    "GENRA MELODY": (("TONE", "BITE", "DETUNE", "AGE"), ("CUTOFF", "RESO", "ENV", "DRIVE"),
                     ("ATTACK", "DECAY", "SUSTAIN", "RELEASE")),
    "GENRA PADS": (("TONE", "ENSEMBLE", "DETUNE", "AGE"), ("CUTOFF", "RESO", "ENV", "DRIVE"),
                   ("ATTACK", "DECAY", "SUSTAIN", "RELEASE")),
}
TONE_MAIN = {
    "GENRA BASS": (("DENSITY", "SWING", "GLIDE", "OCTAVE"), ("CUTOFF", "DRIVE", "DECAY", "LEVEL")),
    "GENRA MELODY": (("DENSITY", "SWING", "RANGE", "OCTAVE"), ("CUTOFF", "ATTACK", "DECAY", "LEVEL")),
    "GENRA PADS": (("DENSITY", "SPREAD", "STRUM", "OCTAVE"), ("CUTOFF", "ATTACK", "RELEASE", "LEVEL")),
}
PLUGIN = ""  # the plugin being drawn (main() sets it)


def tone_header(p, fields=True):
    w = NAME_W.get(PLUGIN, 110)
    p.caption("genra", "20 8 120 48", 34.0, L_DARK)
    p.caption("name", "136 8 %d 48" % (w + 10), 34.0, L_RED)
    if fields:
        x = 140 + w
        lw = 810 - x
        p.dark("%d 9 %d 46" % (x, lw))
        p.value("KOPFLINK", "%d 9 %d 46" % (x + 12, lw - 20), 20.0, L_CREAM)
        p.dark("820 9 440 46")
        p.value("KOPFPRESET", "832 9 420 46", 20.0, L_CREAM)


def tone_sound(p):
    tone_header(p)
    p.radio("KIND", ["cat%d" % (i + 1) for i in range(6)], [20 + i * 69 for i in range(6)], 66, 65, 40, 13.0)
    lit = p.img.two(DARK, SEL, 410, 52)
    for r in range(6):
        y = 116 + r * 58
        p.film(lit, 2, "PRESETLICHT %d" % (r + 1), "20 %d 410 52" % y)
        p.value("PRESETNAME %d" % (r + 1), "36 %d 390 52" % y, 21.0, L_CREAM)
        p.button("PRESET %d" % (r + 1), "20 %d 410 52" % y)
    for gi, (cap, params) in enumerate(zip(("osc", "filter", "env"), TONE_SOUND[PLUGIN])):
        y = 66 + gi * 174
        p.dark("450 %d 810 166" % y)
        p.caption(cap, "466 %d 300 30" % (y + 6), 17.0, L_CREAM_MUTED)
        for i, param in enumerate(params):
            p.knob(param, 466 + i * 198, y + 54, 64, L_CREAM, L_CREAM_MUTED)
    p.lcd("20 586 1240 32")
    p.caption("hint", "32 586 1220 32", 16.0, L_LCD)


def tone_buttons(p):
    p.key("NEW", RED, "new", "20 276 180 60", 26.0)
    p.key("UNDO", GREY, "undo", "208 276 140 60", 22.0, L_KEYINK)
    p.radio("AUTO PADS", ("auto", "padsmode"), (356, 470), 276, 110, 60, 20.0)
    p.toggle("KEY LOCK", "keylock", "588 276 170 60", 20.0)


def tone_knobs(p):
    pattern, tone = TONE_MAIN[PLUGIN]
    p.dark("20 350 610 182")
    p.caption("pattern", "36 354 300 28", 17.0, L_CREAM_MUTED)
    p.dark("650 350 610 182")
    p.caption("tone", "666 354 300 28", 17.0, L_CREAM_MUTED)
    for i, param in enumerate(pattern):
        p.knob(param, 76 + i * 150, 384, 64, L_CREAM, L_CREAM_MUTED, below=True)
    for i, param in enumerate(tone):
        p.knob(param, 706 + i * 150, 384, 64, L_CREAM, L_CREAM_MUTED, below=True)
    p.lcd("20 544 1240 40")
    p.value("STATUS", "32 544 1220 40", 18.0, L_LCD)


def tone_main(p):
    """GENRA BASS and GENRA MELODY: the line on the display, the chords above it."""
    tone_header(p)
    p.lcd("20 64 1240 200")
    col = p.img.line_cols()
    for s in range(64):
        p.film(col, 34, "LINIE %d" % (s + 1), "%d 86 19 176" % (20 + int(round(s * 19.375))))
    for b in range(4):
        p.value("AKKORD %d" % (b + 1), "%d 64 300 22" % (30 + b * 310), 17.0, L_LCD)
    tone_buttons(p)
    p.caption("rhythm", "780 270 200 22", 15.0, L_MUTED)
    p.radio("RHYTHM", ("rh1", "rh2", "rh3"), (780, 870, 960), 294, 86, 42, 17.0)
    p.caption("notes", "1056 270 200 22", 15.0, L_MUTED)
    p.radio("NOTES", ("no1", "no2", "no3"), (1056, 1126, 1196), 294, 66, 42, 15.0)
    tone_knobs(p)


def pads_main(p):
    """GENRA PADS: AUTO shows the chords of the loop; PADS turns the display into the chords of the 16 MPC pads."""
    img = p.img
    tone_header(p)
    p.lcd("20 64 1240 200")
    col = img.chord_cols()
    for s in range(64):
        p.film(col, 20, "LINIE %d" % (s + 1), "%d 90 19 172" % (20 + int(round(s * 19.375))))
    for b in range(4):
        p.value("AKKORD %d" % (b + 1), "%d 64 300 24" % (30 + b * 310), 17.0, L_LCD)
    cell = img.pad_cells(145, 76)
    for i in range(16):
        x = 30 + (i % 8) * 153
        y = 92 + (1 - i // 8) * 84
        p.film(cell, 3, "PADZELLE %d" % (i + 1), "%d %d 145 76" % (x, y))
        p.value("PADNUMMER %d" % (i + 1), "%d %d 130 22" % (x + 8, y + 4), 13.0, "ffb4b7b0")
        p.value("PADAKKORD %d" % (i + 1), "%d %d 145 44" % (x, y + 26), 21.0, "fff0f0ec", CENTRED)
    p.key("NEW", RED, "new", "20 276 150 60", 26.0)
    p.key("UNDO", GREY, "undo", "176 276 110 60", 22.0, L_KEYINK)
    p.radio("AUTO PADS", ("auto", "padsmode"), (292, 384), 276, 88, 60, 19.0)
    p.toggle("KEY LOCK", "keylock", "478 276 140 60", 18.0)
    for k, (cap, param, caps) in enumerate((("rhythm", "RHYTHM", ("rh1", "rh2", "rh3")),
                                            ("voicing", "VOICING", ("vo1", "vo2", "vo3")),
                                            ("padset", "PAD SET", ("ps1", "ps2", "ps3")))):
        x0 = 630 + k * 212
        p.caption(cap, "%d 270 200 22" % x0, 15.0, L_MUTED)
        p.radio(param, caps, (x0, x0 + 68, x0 + 136), 294, 64, 42, 13.0)
    tone_knobs(p)


def tone_print(p):
    tone_header(p, False)
    print_page(p, 140 + NAME_W.get(PLUGIN, 110), False)


def tone_setup(p):
    tone_header(p, False)
    rows = (("r1", "LINKINFO"), ("r2", "TONARTINFO"), ("r3", "PRINTORDNER"), ("r4", "VERSION"))
    for i, (key, param) in enumerate(rows):
        y = 80 + i * 70
        p.dark("20 %d 1240 60" % y)
        p.caption(key, "40 %d 260 60" % y, 24.0, L_CREAM_MUTED)
        p.value(param, "300 %d 940 60" % y, 24.0, L_CREAM)


# ------------------------------------------------------------------ SCHLAG
SCHLAG_FLASH = ["NEW", "UNDO", "BASIC", "PRINT", "LEARN", "FOLDER UP", "DRIVES", "PAGE BACK", "PAGE NEXT",
                "ROOT HERE"]
DEAD_VOICES = {2: 3, 6: 4}  # CLAP and TOM: dying displays (value: the damage pattern)


class SchlagPage(Page):
    flashes = {k: "BLITZ %d" % (i + 1) for i, k in enumerate(SCHLAG_FLASH)}


def schlag_header(p):
    p.caption("schlag", "20 8 170 48", 34.0, L_RED)


def schlag_main(p):
    img = p.img
    schlag_header(p)
    p.lcd("196 9 370 46")
    p.value("KOPFLINK", "208 9 350 46", 19.0, L_LCD)
    p.radio("AUTO PADS", ("auto", "padsmode"), (576, 660), 9, 80, 46, 19.0)
    p.key("UNDO", GREY, "undo", "746 9 120 46", 20.0, L_KEYINK)
    p.key("BASIC", GREY, "basic", "872 9 120 46", 20.0, L_KEYINK)
    p.key("NEW", RED, "new", "998 9 262 46", 24.0)
    lamps, ring, small = img.lamps(), img.ring(72), img.knob(40)
    pad = img.two(PAD, RED_LIT, 145, 62, 7)
    for v in range(8):
        x, w = 20 + v * 155, 145
        dead = DEAD_VOICES.get(v, 0)
        ink = L_DEAD if dead else L_LCD
        p.dark("%d 66 %d 484" % (x, w))
        p.caption("v%d" % (v + 1), "%d 72 110 28" % (x + 10), 17.0 if v == 5 else 20.0, L_CREAM)
        p.film(lamps, 2, "LAMPE %d" % (v + 1), "%d 78 16 16" % (x + w - 22))
        p.toggle("M %d" % (v + 1), "m", "%d 102 62 30" % (x + 8), 17.0)
        p.toggle("S %d" % (v + 1), "s", "%d 102 62 30" % (x + 75), 17.0)
        p.lcd("%d 140 129 30" % (x + 8), dead)
        p.value("ORDNERTEXT %d" % (v + 1), "%d 140 119 30" % (x + 14), 14.0, ink)
        p.lcd("%d 176 129 30" % (x + 8), dead and dead + 1)
        p.value("SAMPLETEXT %d" % (v + 1), "%d 176 119 30" % (x + 14), 15.0, ink)
        p.button("WAHL %d" % (v + 1), "%d 140 129 66" % (x + 8))
        p.lcd("%d 212 129 24" % (x + 8), dead and dead + 2)
        cells = img.drum_cells(bool(dead))
        for j in range(16):
            p.film(cells, 3, "ZELLE %d %d" % (v + 1, j + 1), "%d 215 6 18" % (x + 11 + int(round(j * 7.9))))
        p.knob_only("RANDOM %s" % VOICE_NAMES[v], "%d 244 72 72" % (x + 36), ring)
        p.lcd("%d 322 89 26" % (x + 28), dead)
        p.value("RANDOM %s" % VOICE_NAMES[v], "%d 322 89 26" % (x + 28), 16.0, ink, CENTRED)
        for k in range(4):
            cx, cy = x + 38 + (k % 2) * 69, (412 if k < 2 else 500)
            fb = "%d %d 64 20" % (cx - 32, cy - 48)
            p.lcd(fb, dead)
            p.value("FUNCTION %d %d" % (v + 1, k + 1), fb, 11.0, ink, CENTRED)
            p.button("FUNKTIONSWAHL %d %d" % (v + 1, k + 1), fb)
            p.knob_only("VALUE %d %d" % (v + 1, k + 1), "%d %d 40 40" % (cx - 20, cy - 22), small)
            p.value("VALUE %d %d" % (v + 1, k + 1), "%d %d 68 20" % (cx - 34, cy + 20), 13.0, L_CREAM_MUTED, CENTRED)
        p.film(pad, 2, "PADLICHT %d" % (v + 1), "%d 558 145 62" % x)
        p.caption("p%d" % (v + 1), "%d 560 40 24" % (x + 8), 15.0, L_PAD)
        p.button("PAD %d" % (v + 1), "%d 558 145 62" % x)


def schlag_folder(p):
    schlag_header(p)
    p.caption("ordner", "196 8 300 48", 28.0, L_MUTED)
    p.radio("VOICE", ["v%d" % (i + 1) for i in range(8)], [20 + i * 155 for i in range(8)], 62, 145, 40, 16.0)
    p.dark("20 110 600 46")
    p.value("PFAD", "34 110 580 46", 20.0, L_CREAM)
    for r in range(10):
        y = 162 + r * 42
        p.dark("20 %d 600 38" % y)
        p.value("ZEILE %d" % (r + 1), "34 %d 580 38" % y, 19.0, L_CREAM)
        p.button("TIPP %d" % (r + 1), "20 %d 600 38" % y)
    p.key("PAGE BACK", GREY, "pgup", "20 584 190 36", 18.0, L_KEYINK)
    p.key("PAGE NEXT", GREY, "pgdn", "216 584 190 36", 18.0, L_KEYINK)
    p.key("FOLDER UP", KEY_OFF, "back", "412 584 208 36", 18.0)
    p.dark("640 110 620 250")
    p.value("STIMMINFO", "660 118 580 40", 24.0, L_CREAM)
    p.value("STIMMORDNER", "660 166 580 36", 19.0, L_CREAM_MUTED)
    p.value("STIMMSAMPLE", "660 206 580 36", 19.0, L_CREAM_MUTED)
    p.value("STIMMANZAHL", "660 246 580 36", 19.0, L_CREAM_MUTED)
    p.caption("hint1", "660 290 580 30", 17.0, L_CREAM_MUTED)
    p.caption("hint2", "660 318 580 30", 17.0, L_CREAM_MUTED)
    p.key("DRIVES", KEY_OFF, "drives", "640 372 300 60", 22.0)
    p.key("ROOT HERE", RED, "root", "950 372 310 60", 18.0)
    p.value("LISTINFO", "640 440 620 30", 18.0, L_MUTED, CENTRED)
    p.control("LIST", "811 474 278 107")


def schlag_print(p):
    schlag_header(p)
    print_page(p, 196, False)


def schlag_setup(p):
    schlag_header(p)
    p.caption("setup", "196 8 300 48", 28.0, L_MUTED)
    p.key("LEARN", RED, "learn", "20 70 400 130", 40.0)
    p.value("LERNINFO", "20 210 400 44", 24.0, L_DARK)
    for i in range(4):
        p.caption("help%d" % (i + 1), "20 %d 400 32" % (262 + i * 32), 20.0, L_MUTED)
    p.toggle("HAT CHOKE", "choke", "20 410 400 70", 24.0)
    rows = (("r1", "BELEGUNG"), ("r2", "HAUPTORDNER"), ("r3", "LINKINFO"), ("r4", "SPEICHERINFO"), ("r5", "VERSION"))
    for i, (key, param) in enumerate(rows):
        y = 70 + i * 70
        p.dark("450 %d 810 60" % y)
        p.caption(key, "470 %d 220 60" % y, 22.0, L_CREAM_MUTED)
        p.value(param, "690 %d 560 60" % y, 22.0, L_CREAM)


# ------------------------------------------------------------------ main
def qmap(assign):
    return G.Obj(("Q-Link %d" % q, assign.get(q, -1)) for q in range(1, 17))


def tabmap(n, assigns):
    return [G.obj(("Tab", t + 1), ("SubTab", 1), ("Bank Direction", "Row"), ("Q-Links", qmap(assigns.get(t, {}))))
            for t in range(n)]


def main():
    global PLUGIN
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    project = sys.argv[1]
    with open(os.path.join(project, "plugin.json"), encoding="utf-8") as f:
        plugin = json.load(f)["name"]
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
    idx = {q.name: q.index for q in params}
    table = G.ParamTable(params)
    ui_pages, style, knob_image, frames, kit = G.load_ui(os.path.join(project, "ui.json"), table)
    tui = G.build_tui(ui_pages, params, style, knob_image, frames, {})
    defs = {c.param.name: c.definition for pg in ui_pages for c in pg.controls if c.param is not None}
    page_data = dict(tui)["pageData"]
    tabs = dict(page_data)["tabs"]
    del tabs[:]
    apply_theme(plugin)
    img = Images(out_dir)

    if plugin == "GENRA CHOP":
        cls = ChopPage
        pages = (("BROWSER", chop_browser, ["941 464 298 123"]), ("CHOP", chop_page, ["596 502 664 76"]),
                 ("PRINT", chop_print, []), ("SETUP", chop_setup, []))
        choices = {"BANK": ["A", "B", "C", "D"], "VIEW": ["Sample", "Loop", "Chop"],
                   "MODE": ["Transient", "Equal"], "AUTO PADS": ["Auto", "Pads"]}
        screen = tabmap(4, {0: {13: idx["LIST"]},
                            1: {1: idx["PITCH"], 2: idx["DECAY"], 3: idx["FILTER"], 4: idx["LEVEL"],
                                5: idx["STYLE"], 6: idx["CHOPS"], 7: idx["VOLUME"]}})
        program = qmap({1: idx["PITCH"], 2: idx["DECAY"], 3: idx["FILTER"], 4: idx["LEVEL"], 5: idx["VOLUME"]})
    elif plugin in ("GENRA BASS", "GENRA MELODY", "GENRA PADS"):
        PLUGIN = plugin
        cls = TonePage
        sound_q = ["450 66 810 166", "450 240 810 166", "450 414 810 166"]
        main_q = ["20 350 610 182", "650 350 610 182"]
        lane = {"GENRA BASS": "BASS", "GENRA MELODY": "MELODY", "GENRA PADS": "PADS"}[plugin]
        main_fn = pads_main if plugin == "GENRA PADS" else tone_main
        if plugin == "GENRA BASS":
            pages = (("SOUND", tone_sound, sound_q), (lane, main_fn, main_q), ("PRINT", tone_print, []),
                     ("SETUP", tone_setup, []))
            sound_tab, main_tab = 0, 1
        else:
            pages = ((lane, main_fn, main_q), ("SOUND", tone_sound, sound_q), ("PRINT", tone_print, []),
                     ("SETUP", tone_setup, []))
            sound_tab, main_tab = 1, 0
        kinds = {"GENRA BASS": ["808", "SUB", "UPRIGHT", "E-BASS", "REESE", "ACID"],
                 "GENRA MELODY": ["KEYS", "RHODES", "PLUCK", "LEAD", "BELL", "FLUTE"],
                 "GENRA PADS": ["STRINGS", "CHOIR", "SYNTH", "ORGAN", "E-PIANO", "VINYL"]}[plugin]
        choices = {"KIND": kinds, "AUTO PADS": ["Auto", "Pads"]}
        for q in params:
            if q.name in ("RHYTHM", "NOTES", "VOICING", "PAD SET") and q.choices:
                choices[q.name] = list(q.choices)
        sound = [n for row in TONE_SOUND[plugin] for n in row]
        pattern, tone = TONE_MAIN[plugin]
        screen = tabmap(4, {sound_tab: {i + 1: idx[n] for i, n in enumerate(sound)},
                            main_tab: {i + 1: idx[n] for i, n in enumerate(list(pattern) + list(tone))}})
        program = qmap({i + 1: idx[n] for i, n in enumerate(sound)})
    elif plugin == "SCHLAG":
        cls = SchlagPage
        pages = (("SCHLAG", schlag_main, []), ("FOLDER", schlag_folder, ["801 466 298 123"]),
                 ("PRINT", schlag_print, []), ("SETUP", schlag_setup, []))
        choices = {"AUTO PADS": ["Auto", "Pads"], "VOICE": VOICE_NAMES}
        q = {v + 1: idx["RANDOM %s" % VOICE_NAMES[v]] for v in range(8)}
        q.update({v + 9: idx["LEVEL %s" % VOICE_NAMES[v]] for v in range(8)})
        screen = tabmap(4, {0: q, 1: {13: idx["LIST"]}})
        program = qmap(q)
    else:
        print("build_skin: unknown plugin %s" % plugin)
        return 1

    for i, (name, fn, ql) in enumerate(pages):
        pg = cls(h, style, defs, img)
        fn(pg)
        tabs.append(pg.tab(name, i, ql))
    for k, v in page_data:
        if k == "replacementStrings":
            for param, names in choices.items():
                v.append(G.obj(("key", h[param]), ("value", names)))
            style_p = [q for q in params if q.name == "STYLE"]
            if style_p and style_p[0].choices:
                v.append(G.obj(("key", h["STYLE"]), ("value", list(style_p[0].choices))))
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
    print("build_skin: %s: wrote %s (%d images)" % (plugin, ", ".join(n for n, _, _ in pages), len(img.done)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
