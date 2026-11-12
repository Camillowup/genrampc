#!/usr/bin/env python3
"""add_browser.py - writes CHOP TEST's Plugin Skins with a hand-made BROWSER page in front of the generated ones.

The SDK's gen_ui.py lays out pages of 16 controls from ui.json. The browser needs more: rows you tap, big buttons,
a path bar. This script uses gen_ui's own building blocks (same JSON shapes, same writer with its duplicate
"version" keys), builds the pages from ui.json exactly as gen_ui does, puts the BROWSER tab first and copies the
images from skin-extra/.

Usage (after sdk/tools/build.py has built the plugin):
    python chop-test/tools/add_browser.py "chop-test/CHOP TEST"
"""

import os
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SDK_TOOLS = os.path.join(HERE, "..", "..", "sdk", "tools")
sys.path.insert(0, os.path.abspath(SDK_TOOLS))
import gen_ui as G  # noqa: E402

ROWS = 10


def label(name, kind, handle, bounds, height, colour, justification="left verticallyCentred", text=None):
    remap = [("Data", handle)] if handle else []
    return G.entry(name, "Label", G.label_data(kind, colour, height, justification, text), bounds,
                   "No", "Show", "Always", remap=remap)


def image(name, png, bounds):
    data = G.obj(("version", 2), ("imageType", "Regular"), ("colour", "0"), ("image", png))
    return G.entry(name, "Image", data, bounds, "No", "Show", "Always")


def button(name, png, handle, bounds):
    # a toggle: every tap flips the parameter; the plugin acts on any change. Same image on and off.
    return G.entry(name, "Button", G.button_data(png, png, 1, 1), bounds, "No", "Show", "Always",
                   remap=[("Data", handle)])


def browser_tab(handles, style):
    white, muted, accent = "ffffffff", style["muted"], style["accent"]
    c = []
    # path bar
    c.append(image("path bg", "br_path.png", "20 6 1240 52"))
    c.append(label("path", "Value", handles["PATH"], "38 6 1210 52", 30.0, white))
    # the rows: background, the name, an invisible button on top
    for k in range(ROWS):
        y = 66 + k * 52
        c.append(image("row bg %d" % k, "br_row.png", "20 %d 880 48" % y))
        c.append(label("row %d" % k, "Value", handles["ZEILE %d" % (k + 1)], "38 %d 850 48" % y, 28.0, white))
        c.append(button("row tap %d" % k, "br_clear_row.png", handles["TIPP %d" % (k + 1)], "20 %d 880 48" % y))
    # big buttons on the right
    centred = "horizontallyCentred verticallyCentred"
    for name, png, clear, cap, handle, b in (
            ("back", "br_red.png", "br_clear_big.png", "ZURÜCK", "ZURUECK", "920 66 340 110"),
            ("drives", "br_purple.png", "br_clear_big.png", "LAUFWERKE", "LAUFWERKE BTN", "920 186 340 110"),
            ("page up", "br_grey.png", "br_clear_small.png", "< SEITE", "SEITE ZURUECK", "920 306 165 110"),
            ("page down", "br_grey.png", "br_clear_small.png", "SEITE >", "SEITE VOR", "1095 306 165 110")):
        c.append(image(name + " bg", png, b))
        c.append(label(name + " caption", "Text", None, b, 34.0, white, centred, cap))
        c.append(button(name + " tap", clear, handles[handle], b))
    c.append(label("page info", "Value", handles["SEITE"], "920 430 340 60", 26.0, muted, centred))
    # what is loaded
    c.append(label("loaded caption", "Text", None, "20 584 150 40", 26.0, muted, text="Geladen:"))
    c.append(label("loaded", "Value", handles["GELADEN"], "170 584 1090 40", 28.0, accent))
    return G.component_definition([], c, style["background"])


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    project = sys.argv[1]
    params_path = os.path.join(project, "build", "params.json")
    ui_path = os.path.join(project, "ui.json")
    out_dirs = [os.path.join(project, "build", d, "Plugin Skins") for d in os.listdir(os.path.join(project, "build"))
                if os.path.isdir(os.path.join(project, "build", d, "Plugin Skins"))]
    if not out_dirs:
        print("add_browser: no built plugin folder under %s/build (run sdk/tools/build.py first)" % project)
        return 1
    out_dir = out_dirs[0]

    params = G.load_params(params_path)
    handles = {p.name: p.handle for p in params}
    table = G.ParamTable(params)
    pages, style, knob_image, frames, kit = G.load_ui(ui_path, table)
    kit_dir, kit_assets = G.kit_dir_for(kit, os.path.join(project, "build"), out_dir) if kit else (None, {})
    tui = G.build_tui(pages, params, style, knob_image, frames, kit_assets)
    qlinks = G.build_qlinks(pages, params)

    # BROWSER first: shift the generated tabs one function key to the right
    page_data = dict(tui)["pageData"]
    tabs = dict(page_data)["tabs"]
    for t in tabs:
        for i, (k, v) in enumerate(t):
            if k == "fnKeyIndex":
                t[i] = (k, v + 1)
    tabs.insert(0, G.obj(("version", 1), ("tabName", "BROWSER"), ("fnKeyIndex", 0), ("fnKeySubIndex", 0),
                         ("componentDefinition", browser_tab(handles, style)),
                         ("qlinkBoundsData", list(G.QLINK_BOUNDS))))
    screen = dict(qlinks)["Screen Mode Q-Links"]
    entries = dict(screen)["map"]
    for e in entries:
        for i, (k, v) in enumerate(e):
            if k == "Tab":
                e[i] = (k, v + 1)
    entries.insert(0, G.obj(("Tab", 1), ("SubTab", 1), ("Bank Direction", "Column"),
                            ("Q-Links", G.Obj(("Q-Link %d" % q, -1) for q in range(1, 17)))))

    # images: the kit's (as gen_ui copies them) and ours
    if kit_dir and os.path.abspath(kit_dir) != os.path.abspath(out_dir):
        for img in sorted(G.referenced_images(tui, set())):
            src = os.path.join(kit_dir, img)
            if os.path.isfile(src):
                shutil.copy2(src, os.path.join(out_dir, img))
    extra = os.path.join(project, "skin-extra")
    for f in sorted(os.listdir(extra)):
        shutil.copy2(os.path.join(extra, f), os.path.join(out_dir, f))
    missing = [i for i in G.referenced_images(tui, set()) if not os.path.isfile(os.path.join(out_dir, i))]
    if missing:
        print("add_browser: missing images: %s" % ", ".join(sorted(missing)))
        return 1

    with open(os.path.join(out_dir, "TUI.json"), "w", encoding="utf-8", newline="\n") as f:
        f.write(G.dump_json(tui) + "\n")
    with open(os.path.join(out_dir, "Q-Links.json"), "w", encoding="utf-8", newline="\n") as f:
        f.write(G.dump_json(qlinks) + "\n")
    print("add_browser: BROWSER page added to %s (%d tabs)" % (out_dir, len(tabs)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
