#!/usr/bin/env python3
"""make_images.py - draws the PNGs of CHOP TEST's skin into "CHOP TEST/skin-extra" (needs Pillow).

The look follows GENRA CHOP in VCV Rack: beige body, grey pads, Akai red, a green LCD, dark sections. Buttons are
film strips of two frames (normal, flashed) so the plugin can light them up for a moment when tapped. Images bound
to a button are drawn at their native size (the MPC does not scale them), so every size is made exactly.

    python chop-test/tools/make_images.py
"""

import os
from PIL import Image, ImageDraw

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "CHOP TEST", "skin-extra")

BEIGE_LIGHT = (234, 229, 216)
PAD = (201, 196, 183)
PAD_EDGE = (160, 155, 144)
RED = (200, 40, 45)
YELLOW = (217, 164, 58)
DARK = (62, 62, 66)
DARKER = (44, 44, 47)
LCD = (159, 179, 136)
LCD_EDGE = (78, 92, 64)
BAR = (214, 205, 176)
BORDER = (226, 70, 70)


def lighter(c, k=55):
    return tuple(min(255, v + k) for v in c[:3])


def rect(d, box, fill, outline=None, radius=10, width=2):
    d.rounded_rectangle(box, radius=radius, fill=fill, outline=outline, width=width)


def plain(name, w, h, fill, outline=None, radius=10):
    im = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    rect(ImageDraw.Draw(im), [0, 0, w - 1, h - 1], fill, outline, radius)
    im.save(os.path.join(OUT, name))


def strip2(name, w, h, fill, outline=None, radius=10, flash=None):
    """Two frames stacked vertically: normal, flashed (brighter)."""
    im = Image.new("RGBA", (w, h * 2), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    rect(d, [0, 0, w - 1, h - 1], fill, outline, radius)
    rect(d, [0, h, w - 1, 2 * h - 1], flash or lighter(fill), outline and lighter(outline), radius)
    im.save(os.path.join(OUT, name))


def clear(name, w, h):
    Image.new("RGBA", (w, h), (0, 0, 0, 0)).save(os.path.join(OUT, name))


def wave_strip(name, w=19, h=84, heights=12):
    """heights x {normal, playing} x {no border, border} frames: frame = height + 12 x playing + 24 x border."""
    frames = heights * 4
    im = Image.new("RGBA", (w, h * frames), DARKER + (255,))
    d = ImageDraw.Draw(im)
    for f in range(frames):
        hh, playing, border = f % heights, (f // heights) % 2, f // (2 * heights)
        y0 = f * h
        bar = int(4 + (h - 12) * hh / (heights - 1))
        top = y0 + (h - bar) // 2
        d.rectangle([3, top, w - 4, top + bar], fill=RED if playing else BAR)
        if border:
            d.rectangle([0, y0 + 2, 1, y0 + h - 3], fill=BORDER)
    im.save(os.path.join(OUT, name))


def main():
    os.makedirs(OUT, exist_ok=True)
    for f in os.listdir(OUT):
        if f.endswith(".png"):
            os.remove(os.path.join(OUT, f))
    # panels
    plain("lcd.png", 400, 50, LCD, LCD_EDGE, 8)
    plain("panel_dark.png", 400, 200, DARKER, None, 12)
    plain("setup_row.png", 400, 60, BEIGE_LIGHT, PAD_EDGE, 8)
    # browser
    strip2("row.png", 880, 46, BEIGE_LIGHT, PAD_EDGE, 8, flash=(255, 236, 190))
    strip2("btn_dark_340x80.png", 340, 80, DARK)
    strip2("btn_dark_165x80.png", 165, 80, DARK)
    strip2("btn_yellow_340x70.png", 340, 70, YELLOW)
    # pads
    im = Image.new("RGBA", (140, 200), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    rect(d, [0, 0, 139, 99], PAD, PAD_EDGE, 12)
    rect(d, [0, 100, 139, 199], RED, lighter(RED), 12)
    im.save(os.path.join(OUT, "pad_strip.png"))
    plain("bank_off.png", 140, 60, DARK, None, 10)
    plain("bank_on.png", 140, 60, RED, lighter(RED), 10)
    strip2("btn_yellow_295x60.png", 295, 60, YELLOW)
    strip2("btn_dark_295x60.png", 295, 60, DARK)
    strip2("btn_dark_90x44.png", 90, 44, DARK, None, 8)
    wave_strip("wave_strip.png")
    # setup
    strip2("btn_red_400x130.png", 400, 130, RED)
    # invisible buttons on top of the images (native size)
    for w, h in ((880, 46), (340, 80), (165, 80), (340, 70), (140, 100), (295, 60), (90, 44), (400, 130)):
        clear("clear_%dx%d.png" % (w, h), w, h)
    print("make_images: wrote %d images to %s" % (len([f for f in os.listdir(OUT) if f.endswith('.png')]), OUT))


if __name__ == "__main__":
    main()
