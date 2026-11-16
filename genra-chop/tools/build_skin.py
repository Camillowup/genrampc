#!/usr/bin/env python3
"""build_skin.py - writes the Plugin Skins of GENRA CHOP and GENRA SCHLAG and draws
every image.

Look: the retro MPC (keys, dark fields, LCDs like the MPC 2000); every plugin has the colours of a limited-edition MPC
(THEMES below). The MPC draws images at their native size (nothing is scaled), so every box is drawn here at
exactly the size the layout asks for; the PNGs are written with the standard library only (zlib). Fixed texts in a
skin are not drawn on the MPC: every caption is a read-only parameter the plugin serves ("CAP <key>"). Film strips
(display columns, slots, steps, cells, lamps, flashes) get their frame from the plugin as the parameter's value.

Usage (after sdk/tools/build.py has built the plugin):
    python genra-chop/tools/build_skin.py "genra-chop/GENRA CHOP"     (or "GENRA SCHLAG")
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
    # SCHLAG: black (MPC3000 LE)
    "GENRA SCHLAG": dict(GREY_LCD, DARK=(0x33, 0x33, 0x33), CREAM=(0xE8, 0xE8, 0xE4), GREY=(0x8C, 0x8C, 0x88),
                   KEY_OFF=(0x55, 0x55, 0x52), PAD=(0x6A, 0x6A, 0x66), LANE=(0x44, 0x44, 0x44),
                   KNOB=(0x48, 0x48, 0x48), RED=(0xD3, 0x2F, 0x2F), RED_LIT=(0xE8, 0x4A, 0x40),
                   L_DARK="ffe8e8e4", L_MUTED="ffa8a8a4", L_CREAM="ffe8e8e4", L_CREAM_MUTED="ff9a9a96",
                   L_PAD="fff0f0ec", L_RED="ffd32f2f", L_KEYINK="ff1c1c1c"),
}


def apply_theme(plugin):
    t = THEMES.get(plugin, {})
    if "RED" in t and "SEL" not in t:
        t = dict(t, SEL=t["RED"])
    globals().update(t)


OVERLAY_BG = "ff1e1d1e"  # the overlays' background (the SDK's dark default)
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

    def sel_frame(self, w, h, t=3):
        """Two frames: nothing, and a red frame (the chosen voice's column)."""
        def make():
            off, on = Img(w, h), Img(w, h)
            on.outline(0, 0, w, h, RED, t, 6)
            return strip([off, on])
        return self._write("selframe_%dx%d.png" % (w, h), make)

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
            r_in = c * 0.55
            for yy in range(size):  # a ring: the middle stays see-through (the value lies under it)
                for xx in range(size):
                    d = math.hypot(xx + 0.5 - c, yy + 0.5 - c)
                    if r_in <= d <= c - 1:
                        i.put(xx, yy, KNOB)
            lit = int(round(k / 126.0 * 24))
            for d in range(24):
                a = d / 24.0 * 2 * math.pi
                i.disc(c + (c - 8) * math.sin(a), c - (c - 8) * math.cos(a), 2.6, RED_LIT if d < lit else (0x5A, 0x5C, 0x58))
            return i
        return self._write("ring%d.png" % size, lambda: strip([frame(k) for k in range(127)]))

    def step_cells(self, w, h):
        """STEPS: 144 frames = (((base * 3 + nudge) * 3 + dot) * 2 + own sample) * 2 + selected.
        base: 0/1 empty (beat groups), 2 soft hit, 3 loud hit; nudge: 1 early, 2 late; dot: 1 own values, 2 own RANDOM;
        own sample: a blue square top left."""
        def frame(f):
            sel, smp, dot, nudge, base = f % 2, (f // 2) % 2, (f // 4) % 3, (f // 12) % 3, f // 36
            i = Img(w, h)
            col = ((0x44, 0x44, 0x44), (0x3C, 0x3C, 0x3C), (0xA8, 0xA8, 0xA2), (0xE0, 0xE0, 0xDA))[base]
            i.rect(0, 0, w, h, col, 5)
            if nudge:
                i.rect(6 if nudge == 1 else w - 10, 8, 4, h - 16, RED, 1)
            if dot == 1:
                i.rect(w - 15, 5, 9, 9, RED, 4)
            elif dot == 2:
                i.rect(w // 2 - 14, h - 9, 28, 4, RED, 1)
                i.rect(w - 15, 5, 9, 9, RED, 4)
            if smp:
                i.rect(6, 5, 9, 9, (0x4D, 0xA3, 0xFF), 1)
            if sel:
                blue = (0x4D, 0xA3, 0xFF)
                for x0, y0, ww, hh in ((0, 0, w, 3), (0, h - 3, w, 3), (0, 0, 3, h), (w - 3, 0, 3, h)):
                    i.rect(x0, y0, ww, hh, blue, 0)
            return i
        return self._write("stepcell144_%dx%d_strip.png" % (w, h), lambda: strip([frame(f) for f in range(144)]))

    def play_head(self, w, h):
        def frame(f):
            i = Img(w, h)
            if f:
                for x0, y0, ww, hh in ((0, 0, w, 3), (0, h - 3, w, 3), (0, 0, 3, h), (w - 3, 0, 3, h)):
                    i.rect(x0, y0, ww, hh, RED, 0)
            return i
        return self._write("playhead_%dx%d_strip.png" % (w, h), lambda: strip([frame(f) for f in range(2)]))

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
KNOB_DEFS = {}  # custom components of the film-strip knobs (name -> definition), added to the TUI by main()


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
        """A film-strip knob in its own component, so a double tap (or Enter) opens the MPC's large knob editor."""
        w, h = [int(v) for v in b.split()[2:]]
        name = "genra knob %s %dx%d" % (png[:-4], w, h)
        if name not in KNOB_DEFS:
            KNOB_DEFS[name] = G.component_definition(
                G.control_actions("knob overlay"),
                [G.entry("knob", "Knob", G.knob_filmstrip_data(png, 127), "0 0 %d %d" % (w, h), "No", "Show", "Always")],
                "00000000")
        self.c.append(G.entry("knob %d" % len(self.c), name, G.obj(("version", 1), ("handleName", "Data")), b, "Yes",
                              "Hide", "Always", remap=[("Data", self.h[param])]))

    def knob(self, param, x, y, size=64, name_colour=L_DARK, value_colour=L_MUTED, below=False):
        self.knob_only(param, "%d %d %d %d" % (x, y, size, size), self.img.knob(size))
        if below:
            self.name(param, "%d %d %d 24" % (x - 40, y + size + 2, size + 80), 17.0, name_colour, CENTRED)
            self.value(param, "%d %d %d 22" % (x - 40, y + size + 24, size + 80), 15.0, value_colour, CENTRED)
        else:
            self.name(param, "%d %d 110 28" % (x + size + 8, y + 2), 21.0, name_colour)
            self.value(param, "%d %d 110 28" % (x + size + 8, y + 32), 19.0, value_colour)

    def tab(self, name, index, qlink_bounds, sub=0):
        comp = G.component_definition([], self.c, self.style["background"])
        return G.obj(("version", 1), ("tabName", name), ("fnKeyIndex", index), ("fnKeySubIndex", sub),
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
CHOP_FLASH = ["NEW ALL", "NEW DRUMS", "NEW BASS", "NEW MELODY", "NEW PADS", "NEW CHOPS", "UNDO", "SAVE WAV", "CHOPS MINUS",
              "CHOPS PLUS", "STYLE BACK", "STYLE NEXT", "PRINT", "LEARN", "BACK", "DRIVES", "PAGE BACK",
              "PAGE NEXT", "SAVE HERE", "PRINT HERE", "PRINT STANDARD", "LOAD SAMPLE", "CLEAR", "DIAG",
              "MODE BACK 1", "MODE BACK 2", "MODE BACK 3", "MODE BACK 4",
              "MODE NEXT 1", "MODE NEXT 2", "MODE NEXT 3", "MODE NEXT 4"]


class ChopPage(Page):
    flashes = {k: "BLITZ %d" % (i + 1) for i, k in enumerate(CHOP_FLASH)}


def chop_header(p):
    p.caption("genra", "20 8 120 48", 34.0, L_DARK)
    p.caption("chop", "136 8 110 48", 34.0, L_RED)


def chop_page(p, q2=False):
    img = p.img
    chop_header(p)
    p.dark("250 9 330 46")
    p.value("GELADEN", "262 9 310 46", 22.0, L_CREAM)
    p.dark("590 9 310 46")
    if q2:  # the 2nd press of CHOP: the same page, the second Q-Link set (GRID, PAT, OCT, LENGTH, VIEW, BANK, MODE)
        p.caption("qlpage2", "590 9 310 46", 18.0, L_CREAM, CENTRED)
    else:
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
    # the CHOPS row (PAT < > and NEW CHOPS), then per lane: what it plays, MODE < > (DRUMS: the groove), DENSITY
    # (Q-Links 9-12), NEW; PAT, MODE and DENSITY act with the next NEW
    dens = img.knob(48)
    y = 292
    p.caption("chopsl", "32 %d 160 26" % y, 18.0, L_CREAM)
    p.value("CHOPTEXT", "32 %d 160 22" % (y + 28), 12.0, L_CREAM_MUTED)
    p.key("PAT BACK", GREY, "prev", "196 %d 34 50" % (y + 1), 22.0, L_KEYINK)
    p.lcd("234 %d 108 50" % (y + 1))
    p.value("CHOP PAT", "234 %d 108 50" % (y + 1), 15.0, L_LCD, CENTRED)
    p.key("PAT NEXT", GREY, "next", "346 %d 34 50" % (y + 1), 22.0, L_KEYINK)
    # GRID (0.12): the new chops on 1/4, 1/8, 1/16 or AUTO (the style); where the lanes have DENSITY
    p.knob_only("GRID", "390 %d 48 48" % (y + 2), dens)
    p.value("GRID", "444 %d 52 50" % (y + 1), 15.0, L_CREAM, CENTRED)
    p.key("NEW CHOPS", RED, "new", "500 %d 72 50" % (y + 1), 18.0)
    modes = ("DRUM MODE", "BASS MODE", "MELODY MODE", "PADS MODE")
    for k, cap in enumerate(("drums", "bass", "mel", "pads")):
        y = 292 + (k + 1) * 62
        if k == 1:  # BASS: OCT -1 / 0 / +1 / +2 beside the name (light grey like the other keys, tap = next)
            p.caption(cap, "32 %d 64 26" % y, 18.0, L_CREAM)
            p.image(p.img.box(GREY, 92, 26), "100 %d 92 26" % (y + 1))
            p.value("BASS OCT", "100 %d 92 26" % (y + 1), 14.0, L_KEYINK, CENTRED)
            p.button("OCT TAP", "100 %d 92 26" % (y + 1))
        else:
            p.caption(cap, "32 %d 160 26" % y, 18.0, L_CREAM)
        p.value("SPURTEXT %d" % (k + 1), "32 %d 160 22" % (y + 28), 12.0, L_CREAM_MUTED)
        p.key("MODE BACK %d" % (k + 1), GREY, "prev", "196 %d 34 50" % (y + 1), 22.0, L_KEYINK)
        p.lcd("234 %d 108 50" % (y + 1))
        p.value(modes[k], "234 %d 108 50" % (y + 1), 15.0, L_LCD, CENTRED)
        p.key("MODE NEXT %d" % (k + 1), GREY, "next", "346 %d 34 50" % (y + 1), 22.0, L_KEYINK)
        p.knob_only("DENS %s" % ("DRUMS", "BASS", "MELODY", "PADS")[k], "390 %d 48 48" % (y + 2), dens)
        p.value("DENS %s" % ("DRUMS", "BASS", "MELODY", "PADS")[k], "444 %d 52 50" % (y + 1), 13.0, L_CREAM, CENTRED)
        p.key("NEW %s" % ("DRUMS", "BASS", "MELODY", "PADS")[k], RED, "new", "500 %d 72 50" % (y + 1), 18.0)
    p.value("TAKT", "32 600 300 18", 13.0, L_CREAM_MUTED)
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
    p.lcd("600 586 280 32")
    p.value("STATUS", "608 586 268 32", 14.0, L_LCD)
    p.toggle("REC", "rec", "888 586 102 32", 16.0)
    p.key("CLEAR", GREY, "clear", "998 586 122 32", 16.0, L_KEYINK)
    p.key("SAVE WAV", GREY, "savewav", "1128 586 132 32", 16.0, L_KEYINK)


def chop_browser(p):
    p.dark("20 6 1240 52")
    p.value("PFAD", "38 6 1210 52", 28.0, L_CREAM)
    for k in range(9):
        y = 66 + k * 52
        p.dark("20 %d 880 48" % y)
        p.value("ZEILE %d" % (k + 1), "38 %d 850 48" % y, 26.0, L_CREAM)
        p.button("TIPP %d" % (k + 1), "20 %d 880 48" % y)
    p.key("PAGE BACK", GREY, "pgup", "20 536 435 42", 24.0, L_KEYINK)
    p.key("PAGE NEXT", GREY, "pgdn", "465 536 435 42", 24.0, L_KEYINK)
    p.key("BACK", RED, "back", "920 66 340 90", 32.0)
    p.key("DRIVES", KEY_OFF, "drives", "920 164 340 90", 32.0)
    p.key("SAVE HERE", KEY_OFF, "savehere", "920 262 340 70", 24.0)
    p.key("LOAD SAMPLE", RED, "loadsmp", "920 340 340 86", 30.0)
    p.value("LISTINFO", "920 434 340 36", 22.0, L_MUTED, CENTRED)
    p.control("LIST", "951 472 278 107")
    p.caption("loaded", "20 584 150 40", 24.0, L_MUTED)
    p.value("GELADEN", "170 584 1090 40", 26.0, L_RED)


STYLE_CHOICES = []  # "80s · Electro", ... (main() fills it from the plugin's STYLE parameter)


def chop_style(p):
    """STYLE: all styles as keys, one column per decade; a tap rolls a new loop in that style (end of the pattern)."""
    chop_header(p)
    p.caption("styletitle", "250 8 140 48", 26.0, L_MUTED)
    p.lcd("390 9 550 46")
    p.value("STYLEKOPF", "404 9 530 46", 21.0, L_LCD)
    p.key("NEW ALL", RED, "newall", "950 9 310 46", 26.0)
    eras = []
    for i, c in enumerate(STYLE_CHOICES):
        era = c.split(" · ")[0]
        if not eras or eras[-1][0] != era:
            eras.append((era, []))
        eras[-1][1].append(i)
    n, cw = len(STYLE_CHOICES), 244
    for col, (era, idxs) in enumerate(eras):
        x = 20 + col * (cw + 5)
        p.dark("%d 66 %d 480" % (x, cw))
        p.caption("era%d" % col, "%d 70 %d 34" % (x + 12, cw - 20), 20.0, L_CREAM)
        for j, i in enumerate(idxs):
            b = "%d %d %d 56" % (x + 8, 108 + j * 62, cw - 16)
            p.film(p.img.state(i, n, cw - 16, 56), n, "STYLE", b)
            p.value("STYLENAME %d" % (i + 1), b, 20.0, L_WHITE, CENTRED)
            p.button("STYLE", b, button_id=i, group=n)
    p.key("STYLE BACK", GREY, "stprev", "20 556 300 62", 24.0, L_KEYINK)
    p.key("STYLE NEXT", GREY, "stnext", "330 556 300 62", 24.0, L_KEYINK)
    p.caption("length", "650 556 110 62", 18.0, L_MUTED)
    p.radio("LENGTH", ("lenauto", "len1", "len2", "len4", "len8"), (760, 860, 960, 1060, 1160), 556, 96, 62, 17.0)


def chop_print(p):
    chop_header(p)
    print_page(p, 252, True)


def chop_setup(p):
    p.key("LEARN", RED, "learn", "20 20 400 140", 42.0)
    p.value("LERNINFO", "20 175 400 50", 26.0, L_DARK)
    for i in range(4):
        p.caption("help%d" % (i + 1), "20 %d 400 34" % (238 + i * 34), 22.0, L_MUTED)
    rows = (("r1", "BELEGUNG"), ("r2", "LAUFWERKSLISTE"), ("r3", "SCHREIBEN"), ("r4", "LINKINFO"),
            ("r5", "ANALYSE"), ("r6", "SPEICHERORT"), ("r7", "HOSTINFO"), ("r8", "VERSION"))
    for i, (key, param) in enumerate(rows):
        y = 20 + i * 70
        p.dark("450 %d 810 60" % y)
        p.caption(key, "470 %d 220 60" % y, 24.0, L_CREAM_MUTED)
        p.value(param, "690 %d 560 60" % y, 24.0, L_CREAM)
    # DIAG: a 2-minute capture of what the MPC offers and sends, written to Genra/logs/ (genra_diag.h)
    p.key("DIAG", RED, "diag", "20 400 400 90", 30.0)
    p.value("DIAGINFO", "20 500 400 50", 20.0, L_DARK)


# ------------------------------------------------------------------ SCHLAG
SCHLAG_FLASH = ["NEW", "UNDO", "BASIC", "PRINT", "LEARN", "FOLDER UP", "DRIVES", "PAGE BACK", "PAGE NEXT",
                "ROOT HERE", "SAVE SAMPLE", "SAVE KIT", "LOAD SAMPLE", "CLEAR STEP", "SAMPLE BACK", "SAMPLE NEXT",
                "CONDITION BACK", "CONDITION NEXT", "HIT", "CLEAR ALL", "EMPTY", "LOAD KIT",
                "FN BACK 1", "FN NEXT 1", "FN BACK 2", "FN NEXT 2"]
DEAD_VOICES = {6: 4}  # TOM: a dying display (value: the damage pattern)


class SchlagPage(Page):
    flashes = {k: "BLITZ %d" % (i + 1) for i, k in enumerate(SCHLAG_FLASH)}


def schlag_header(p):
    p.caption("schlag", "20 8 170 48", 34.0, L_RED)


def schlag_main(p, funcs=False, all_slots=False):
    img = p.img
    schlag_header(p)
    p.lcd("196 9 370 46")
    if all_slots:  # the 3rd press: the Q-Links on the function slots of all eight voices
        p.caption("qlall", "208 9 350 46", 19.0, L_LCD)
    elif funcs:  # the 2nd press of the function key: the Q-Links on the chosen voice (tap its name)
        p.value("QLTEXT", "208 9 350 46", 19.0, L_LCD)
    else:
        p.value("KOPFLINK", "208 9 350 46", 19.0, L_LCD)
    p.radio("AUTO PADS", ("auto", "padsmode"), (576, 660), 9, 80, 46, 19.0)
    p.key("UNDO", GREY, "undo", "746 9 120 46", 20.0, L_KEYINK)
    p.key("BASIC", GREY, "basic", "872 9 120 46", 20.0, L_KEYINK)
    p.key("NEW", RED, "new", "998 9 262 46", 24.0)
    # 0.10: no folder / sample fields and no pads: < > per voice steps through its folder's samples (the name shows
    # in the header), a tap on the voice's name chooses it for the FOLDER page; RANDOM 100 px with its value under
    # the ring (the ring's middle is see-through, so a tap is the knob's); slots with a smaller field, knob 60.
    lamps, ring, small = img.lamps(), img.ring(100), img.knob(60)
    for v in range(8):
        x, w = 20 + v * 155, 145
        dead = DEAD_VOICES.get(v, 0)
        ink = L_DEAD if dead else L_LCD
        p.dark("%d 66 %d 554" % (x, w))
        p.film(img.sel_frame(w, 554), 2, "STIMMRAHMEN %d" % (v + 1), "%d 66 %d 554" % (x, w))
        # the name: white, or red while it is the chosen voice (Q-LINKS: FUNCTIONS and FOLDER follow it)
        p.value("STIMMNAME %d" % (v + 1), "%d 70 110 30" % (x + 10), 17.0 if v == 5 else 20.0, L_CREAM)
        p.value("STIMMNAME ROT %d" % (v + 1), "%d 70 110 30" % (x + 10), 17.0 if v == 5 else 20.0, L_RED)
        p.film(lamps, 2, "LAMPE %d" % (v + 1), "%d 77 16 16" % (x + w - 22))
        p.button("WAHL %d" % (v + 1), "%d 66 %d 34" % (x, w))
        p.toggle("M %d" % (v + 1), "m", "%d 102 62 30" % (x + 8), 17.0)
        p.toggle("S %d" % (v + 1), "s", "%d 102 62 30" % (x + 75), 17.0)
        p.key("VOICE PREV %d" % (v + 1), GREY, "lt", "%d 140 62 52" % (x + 8), 26.0, L_KEYINK)
        p.key("VOICE NEXT %d" % (v + 1), GREY, "gt", "%d 140 62 52" % (x + 75), 26.0, L_KEYINK)
        p.lcd("%d 200 129 24" % (x + 8), dead and dead + 2)
        cells = img.drum_cells(bool(dead))
        for j in range(16):
            p.film(cells, 3, "ZELLE %d %d" % (v + 1, j + 1), "%d 203 6 18" % (x + 11 + int(round(j * 7.9))))
        # the value first, the ring on top of it: its see-through middle shows the value, a tap reaches the knob
        p.value("RINGTEXT %d" % (v + 1), "%d 260 60 30" % (x + 42), 17.0, L_CREAM, CENTRED)
        p.knob_only("RANDOM %s" % VOICE_NAMES[v], "%d 226 100 100" % (x + 22), ring)
        # what RANDOM does (tap = the next kind)
        p.lcd("%d 334 62 26" % (x + 8), dead)
        p.value("KIND %s" % VOICE_NAMES[v], "%d 334 62 26" % (x + 8), 12.0, ink, CENTRED)
        p.button("KIND TAP %d" % (v + 1), "%d 334 62 26" % (x + 8))
        # HOLD | LIVE: grey = HOLD (the roll stays), red = LIVE (every pass rolls again); the text is the value
        hb = "%d 334 62 26" % (x + 75)
        p.film(img.state(1, 2, 62, 26), 2, "LIVE %s" % VOICE_NAMES[v], hb)
        p.value("LIVE %s" % VOICE_NAMES[v], hb, 15.0, L_WHITE, CENTRED)
        p.button("LIVE %s" % VOICE_NAMES[v], hb, 1, 1)
        # two function slots: the name field (tap = next function), a bigger knob, its value beside it
        for k in range(2):
            y = 370 + k * 122
            fb = "%d %d 129 30" % (x + 8, y)
            p.lcd(fb, dead)
            p.value("FUNCTION %d %d" % (v + 1, k + 1), fb, 16.0, ink, CENTRED)
            p.button("FUNKTIONSWAHL %d %d" % (v + 1, k + 1), fb)
            p.knob_only("VALUE %d %d" % (v + 1, k + 1), "%d %d 60 60" % (x + 8, y + 38), small)
            p.value("VALUE %d %d" % (v + 1, k + 1), "%d %d 70 60" % (x + 70, y + 38), 16.0, L_CREAM, CENTRED)


def schlag_main_funcs(p):
    schlag_main(p, True)


def schlag_main_all(p):
    schlag_main(p, False, True)


def schlag_steps(p):
    """STEPS: GENRA's drum pattern, 8 voices x 16 steps of a bar; tap a step to give it its own values."""
    img = p.img
    schlag_header(p)
    p.caption("stepstitle", "180 8 150 48", 24.0, L_MUTED)
    p.radio("BAR", ("bar1", "bar2", "bar3", "bar4"), (340, 440, 540, 640), 10, 96, 44, 18.0)
    p.key("CLEAR ALL", RED, "clearall", "752 10 130 44", 17.0)
    p.radio("AUTO PADS", ("auto", "padsmode"), (896, 986), 10, 86, 44, 17.0)  # GENRA | TRACK (= the main page's)
    p.lcd("1084 10 176 44")
    p.value("STEPPOS", "1090 10 166 44", 15.0, L_LCD)
    # 16 steps in 4 groups of 4: a wider gap with a line between the groups
    sx = lambda j: 132 + j * 68 + (j // 4) * 12
    cells, head = img.step_cells(62, 44), img.play_head(66, 398)
    for g in (1, 2, 3):
        p.box(KEY_OFF, "%d 66 4 394" % (sx(4 * g) - 9), 0)
    for v in range(8):
        y = 66 + v * 50
        p.dark("20 %d 104 44" % y)
        p.caption("v%d" % (v + 1), "30 %d 94 44" % y, 15.0, L_CREAM)
        for j in range(16):
            p.film(cells, 144, "SCHRITT %d %d" % (v + 1, j + 1), "%d %d 62 44" % (sx(j), y))
    for j in range(16):
        p.film(head, 2, "KOPFSPALTE %d" % (j + 1), "%d 64 66 398" % (sx(j) - 2))
    for v in range(8):
        p.button("LANE TAP %d" % (v + 1), "20 %d 104 44" % (66 + v * 50))  # the name: the whole lane in the editor
        for j in range(16):
            p.button("STEP TAP %d %d" % (v + 1, j + 1), "%d %d 62 44" % (sx(j), 66 + v * 50))
    y = 474
    p.dark("20 %d 1240 146" % y)
    p.value("STEPKOPF", "34 %d 400 28" % (y + 4), 16.0, L_CREAM)
    # 0.12: two knobs, each with a field whose < > choose what it sets for the step (like the main page's slots):
    # SAMPLE, CONDITION, LEVEL, CHANCE, RANDOM, TUNE .. FILTER; its value beside the knob
    knob = img.knob(52)
    for k, kx in enumerate((60, 470)):
        n = k + 1
        p.key("FN BACK %d" % n, GREY, "lt", "%d %d 40 40" % (kx, y + 40), 20.0, L_KEYINK)
        p.lcd("%d %d 220 40" % (kx + 46, y + 40))
        p.value("STEPFN %d" % n, "%d %d 220 40" % (kx + 46, y + 40), 18.0, L_LCD, CENTRED)
        p.key("FN NEXT %d" % n, GREY, "gt", "%d %d 40 40" % (kx + 272, y + 40), 20.0, L_KEYINK)
        p.knob_only("STEP KNOB %d" % n, "%d %d 52 52" % (kx + 14, y + 86), knob)
        p.value("STEPFNWERT %d" % n, "%d %d 230 40" % (kx + 80, y + 92), 17.0, L_CREAM)
    p.key("HIT", RED, "hit1", "1150 %d 100 57" % (y + 13), 15.0)
    p.caption("hit2", "1150 %d 100 20" % (y + 44), 12.0, L_WHITE, CENTRED)
    p.key("CLEAR STEP", GREY, "clearstep", "1150 %d 100 57" % (y + 76), 14.0, L_KEYINK)


def schlag_folder(p):
    schlag_header(p)
    p.caption("ordner", "196 8 300 48", 28.0, L_MUTED)
    p.radio("VOICE", ["v%d" % (i + 1) for i in range(8)], [20 + i * 155 for i in range(8)], 62, 145, 40, 16.0)
    p.dark("20 110 600 46")
    p.value("PFAD", "34 110 580 46", 22.0, L_CREAM)
    for r in range(8):
        y = 162 + r * 52
        p.dark("20 %d 600 48" % y)
        p.value("ZEILE %d" % (r + 1), "34 %d 580 48" % y, 24.0, L_CREAM)
        p.button("TIPP %d" % (r + 1), "20 %d 600 48" % y)
    p.key("PAGE BACK", GREY, "pgup", "20 584 295 36", 18.0, L_KEYINK)
    p.key("PAGE NEXT", GREY, "pgdn", "325 584 295 36", 18.0, L_KEYINK)
    p.dark("640 110 620 110")
    p.value("STIMMINFO", "660 114 300 40", 24.0, L_CREAM)
    p.value("STIMMANZAHL", "960 114 284 40", 17.0, L_CREAM_MUTED)
    p.value("STIMMORDNER", "660 150 580 32", 18.0, L_CREAM_MUTED)
    p.value("STIMMSAMPLE", "660 182 580 32", 18.0, L_CREAM_MUTED)
    p.key("FOLDER UP", RED, "back", "640 230 200 70", 24.0)
    p.key("DRIVES", KEY_OFF, "drives", "850 230 200 70", 22.0)
    p.key("ROOT HERE", KEY_OFF, "root", "1060 230 200 70", 20.0)
    p.key("LOAD SAMPLE", RED, "loadsmp", "640 310 410 80", 28.0)
    p.key("EMPTY", KEY_OFF, "empty", "1060 310 200 80", 24.0)
    p.value("LISTINFO", "640 410 620 50", 18.0, L_MUTED, CENTRED)
    p.key("SAVE SAMPLE", KEY_OFF, "savesmp", "640 476 160 104", 17.0)
    p.control("LIST", "811 474 278 107")
    p.key("SAVE KIT", KEY_OFF, "savekit", "1100 476 160 50", 17.0)
    p.key("LOAD KIT", RED, "loadkit", "1100 530 160 50", 17.0)


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
    p.toggle("MPC KIT NOTES", "kitnotes", "450 440 400 70", 22.0)
    for i in range(3):
        p.caption("kithelp%d" % (i + 1), "870 %d 390 28" % (440 + i * 28), 17.0, L_MUTED)
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
        STYLE_CHOICES[:] = [list(q.choices) for q in params if q.name == "STYLE"][0]
        pages = (("BROWSER", chop_browser, ["941 464 298 123"], 0, 0), ("CHOP", chop_page, ["596 502 664 76"], 1, 0),
                 ("CHOP", lambda pg: chop_page(pg, True), ["596 502 664 76"], 1, 1),
                 ("STYLE", chop_style, [], 2, 0), ("PRINT", chop_print, [], 3, 0), ("SETUP", chop_setup, [], 4, 0))
        choices = {"BANK": ["A", "B", "C", "D"], "VIEW": ["Sample", "Loop", "Chop"],
                   "MODE": ["Transient", "Equal"], "AUTO PADS": ["Auto", "Pads"],
                   "BASS OCT": ["OCT -1", "OCT 0", "OCT +1", "OCT +2"],
                   "CHOP PAT": ["AUTO", "ORDER", "STABS", "CALL+RESP", "HOLD", "MELODY", "HALF ORIG", "SPARSE"],
                   "LENGTH": ["Auto", "1 bar", "2 bars", "4 bars", "8 bars"],
                   "REC": ["Off", "On"], "DRUM MODE": ["STYLE", "HALF", "DOUBLE", "LAID BACK", "DRUNK", "MINIMAL"],
                   "BASS MODE": ["FOLLOW", "ROOT+5", "WALK", "808"], "MELODY MODE": ["STAB", "LEAD", "ARP", "RIFF"],
                   "PADS MODE": ["SUSTAIN", "STAB", "SWELL"], "GRID": ["AUTO", "1 BAR", "1/2", "1/4", "1/8", "1/16"]}
        # CHOP: row 3 DENSITY, row 4 MODE of the four lanes (each lane one column); 8 PAT. The 2nd press of CHOP: GRID,
        # PAT, BASS OCT, LENGTH, VIEW, BANK, MODE (Transient / Equal), rows 3 and 4 as on the first set
        lanes = {9: idx["DENS DRUMS"], 10: idx["DENS BASS"], 11: idx["DENS MELODY"], 12: idx["DENS PADS"],
                 13: idx["DRUM MODE"], 14: idx["BASS MODE"], 15: idx["MELODY MODE"], 16: idx["PADS MODE"]}
        q1 = dict(lanes)
        q1.update({1: idx["PITCH"], 2: idx["DECAY"], 3: idx["FILTER"], 4: idx["LEVEL"],
                   5: idx["STYLE"], 6: idx["CHOPS"], 7: idx["VOLUME"], 8: idx["CHOP PAT"]})
        q2 = dict(lanes)
        q2.update({1: idx["GRID"], 2: idx["CHOP PAT"], 3: idx["BASS OCT"], 4: idx["LENGTH"],
                   5: idx["VIEW"], 6: idx["BANK"], 7: idx["MODE"]})
        screen = [G.obj(("Tab", key + 1), ("SubTab", sub + 1), ("Bank Direction", "Row"), ("Q-Links", qmap(a)))
                  for key, sub, a in ((0, 0, {13: idx["LIST"]}), (1, 0, q1), (1, 1, q2),
                                      (2, 0, {1: idx["STYLE"], 2: idx["LENGTH"], 3: idx["VOLUME"]}), (3, 0, {}), (4, 0, {}))]
        program = qmap({1: idx["PITCH"], 2: idx["DECAY"], 3: idx["FILTER"], 4: idx["LEVEL"], 5: idx["VOLUME"]})
    elif plugin == "GENRA SCHLAG":
        cls = SchlagPage
        # the 2nd press of SCHLAG's function key: the same page, the Q-Links on the function slots (sub-tab)
        pages = (("SCHLAG", schlag_main, [], 0, 0), ("SCHLAG", schlag_main_funcs, [], 0, 1),
                 ("SCHLAG", schlag_main_all, [], 0, 2),
                 ("FOLDER", schlag_folder, ["801 466 298 123"], 1, 0), ("STEPS", schlag_steps, [], 2, 0),
                 ("PRINT", schlag_print, [], 3, 0), ("SETUP", schlag_setup, [], 4, 0))
        choices = {"AUTO PADS": ["GENRA", "TRACK"], "VOICE": VOICE_NAMES, "BAR": ["1", "2", "3", "4"]}
        q = {v + 1: idx["RANDOM %s" % VOICE_NAMES[v]] for v in range(8)}
        q.update({v + 9: idx["LEVEL %s" % VOICE_NAMES[v]] for v in range(8)})
        # Q-LINKS: FUNCTIONS: the chosen voice's column: SAMPLE, RANDOM, upper + lower slot, LEVEL (stand-ins)
        qf = {i + 1: idx["Q %s" % n] for i, n in enumerate(["SAMPLE", "RANDOM", "UPPER", "LOWER", "LEVEL", "KIND"])}
        qf[8] = idx["VOICE"]  # turn = choose the voice
        # the 3rd press: the upper (1-8) and the lower (9-16) function slot of all eight voices
        qa = {v + 1: idx["VALUE %d 1" % (v + 1)] for v in range(8)}
        qa.update({v + 9: idx["VALUE %d 2" % (v + 1)] for v in range(8)})
        qs = {1: idx["STEP KNOB 1"], 2: idx["STEP KNOB 2"], 3: idx["BAR"],  # STEPS: the two knobs, BAR,
              5: idx["STEP FN 1"], 6: idx["STEP FN 2"]}                     # what the two knobs set
        screen = [G.obj(("Tab", key + 1), ("SubTab", sub + 1), ("Bank Direction", "Row"), ("Q-Links", qmap(a)))
                  for key, sub, a in ((0, 0, q), (0, 1, qf), (0, 2, qa), (1, 0, {13: idx["LIST"]}), (2, 0, qs),
                                      (3, 0, {}), (4, 0, {1: idx["VOLUME"]}))]
        program = qmap(q)
    else:
        print("build_skin: unknown plugin %s" % plugin)
        return 1

    for i, pdef in enumerate(pages):
        name, fn, ql = pdef[:3]
        key, sub = (pdef[3], pdef[4]) if len(pdef) > 3 else (i, 0)
        pg = cls(h, style, defs, img)
        fn(pg)
        tabs.append(pg.tab(name, key, ql, sub))
    for k, v in page_data:
        if k == "componentDefinitions":
            local = dict(v)["localComponentDefinitions"]
            for name, comp in KNOB_DEFS.items():
                local.append(G.obj(("key", name), ("value", comp)))
        if k == "replacementStrings":
            for param, names in choices.items():
                v.append(G.obj(("key", h[param]), ("value", names)))
            style_p = [q for q in params if q.name == "STYLE"]
            if style_p and style_p[0].choices:
                v.append(G.obj(("key", h["STYLE"]), ("value", list(style_p[0].choices))))
    # 0.12: the MPC's overlays (the large knob on a double tap, menus) fill the whole screen with their background:
    # keep them dark (GENRA CHOP's cream page made the whole screen white)
    for k, v in page_data:
        if k == "componentDefinitions":
            for e in dict(v)["localComponentDefinitions"]:
                d = dict(e)
                if d["key"] in ("knob overlay", "knob overlay bipolar", "menu overlay"):
                    comp = d["value"]
                    for i, (ck, cv) in enumerate(comp):
                        if ck == "backgroundData":
                            comp[i] = (ck, G.background_data(OVERLAY_BG))
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
    print("build_skin: %s: wrote %s (%d images)" % (plugin, ", ".join(pd[0] for pd in pages), len(img.done)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
