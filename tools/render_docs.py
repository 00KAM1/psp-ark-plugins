#!/usr/bin/env python3
"""Draws the pictures in docs/images.

A stand-in scene (drawn here; not a real game) is run through the plugin's own
pixel code (scanline/src/lines.c, by way of tools/render_tool) and its own label code and
font, so the lines and the label are exactly what the PSP shows. Each PSP pixel
is shown as 2x2, or larger in the close-ups.

  gcc -O2 -std=gnu99 -Iscanline/tests/shim -include scanline/tests/shim/psptypes.h -o /tmp/render_tool tools/render_tool.c scanline/src/lines.c
  python3 tools/render_docs.py /tmp/render_tool          (needs Pillow)
"""
import os, subprocess, sys, tempfile
from PIL import Image, ImageDraw
W, H = 480, 272
HERE = os.path.dirname(os.path.abspath(__file__))
TOOL = sys.argv[1]
FONTP = os.path.join(HERE, "msx.bin")
OUT = os.path.join(HERE, "..", "docs", "images")
TMP = tempfile.mkdtemp()
font = open(FONTP, "rb").read()
LEVEL = {"12.5": 0, "25": 1, "37.5": 2, "50": 3, "62.5": 4, "75": 5, "100": 6}

def put_text(im, x, y, s, col, scale=1):
    d = ImageDraw.Draw(im)
    for n, ch in enumerate(s):
        g = font[ord(ch) * 8: ord(ch) * 8 + 8]
        for r in range(8):
            for c in range(8):
                if g[r] & (0x80 >> c):
                    x0, y0 = x + (n * 8 + c) * scale, y + r * scale
                    d.rectangle([x0, y0, x0 + scale - 1, y0 + scale - 1], fill=col)

def scene():
    """PIL polygons are not anti-aliased, which gives PSP-like stair steps."""
    im = Image.new("RGB", (W, H)); d = ImageDraw.Draw(im)
    for y in range(150):
        t = y / 150; d.line([(0, y), (W, y)], fill=(int(40 + 200 * t), int(70 + 90 * t), int(170 - 60 * t)))
    d.ellipse([360, 40, 410, 90], fill=(255, 220, 120))
    d.polygon([(-20, 150), (90, 70), (200, 150)], fill=(90, 80, 120))
    d.polygon([(120, 150), (260, 52), (420, 150)], fill=(70, 70, 105))
    d.polygon([(330, 150), (430, 95), (520, 150)], fill=(95, 90, 130))
    d.polygon([(260, 52), (232, 72), (288, 72)], fill=(235, 235, 245))
    horizon = 150
    d.rectangle([0, horizon, W, H], fill=(40, 110, 55))
    zs = [3.2 * 1.06 ** i for i in range(140)]
    for i in range(len(zs) - 1):
        za, zb = zs[i], zs[i + 1]
        ya, yb = horizon + 400 / za, horizon + 400 / zb
        for k in range(-16, 17):
            xa0, xa1 = W / 2 + k * 180 / za, W / 2 + (k + 1) * 180 / za
            xb0, xb1 = W / 2 + k * 180 / zb, W / 2 + (k + 1) * 180 / zb
            if (i + k) % 2 == 0: d.polygon([(xa0, ya), (xa1, ya), (xb1, yb), (xb0, yb)], fill=(60, 150, 70))
    d.polygon([(300, 205), (352, 186), (370, 236), (318, 256)], fill=(150, 100, 50))
    d.line([(300, 205), (370, 236)], fill=(90, 60, 30), width=2); d.line([(352, 186), (318, 256)], fill=(90, 60, 30), width=2)
    d.ellipse([140, 168, 162, 190], fill=(240, 200, 160))
    d.polygon([(138, 192), (164, 192), (170, 238), (132, 238)], fill=(200, 40, 50))
    d.line([(170, 200), (214, 160)], fill=(220, 220, 235), width=3)
    d.line([(132, 238), (128, 262)], fill=(50, 50, 90), width=5); d.line([(168, 238), (174, 262)], fill=(50, 50, 90), width=5)
    d.rectangle([8, 8, 124, 26], fill=(20, 20, 40)); put_text(im, 14, 13, "HP 120  MP 45", (255, 255, 255))
    return im

SRC = scene()
open(os.path.join(TMP, "scene.rgb"), "wb").write(SRC.tobytes())

def run(strength=None, spacing=2, labels=()):
    """strength None = scanlines off (the label can still be drawn)."""
    level = -1 if strength is None else LEVEL[strength]
    out = os.path.join(TMP, "out.rgb")
    subprocess.check_call([TOOL, os.path.join(TMP, "scene.rgb"), out, "3", str(level), str(spacing), FONTP] + list(labels))
    return Image.frombytes("RGB", (W, H), open(out, "rb").read())

BG, FG = (18, 18, 22), (232, 232, 232)
def tile(im, caption, scale=2, box=None):
    if box: im = im.crop(box)
    im = im.resize((im.width * scale, im.height * scale), Image.NEAREST)
    out = Image.new("RGB", (im.width, im.height + 28), BG)
    out.paste(im, (0, 28)); put_text(out, 8, 6, caption, FG, 2)
    return out
def sheet(tiles, cols, name):
    tw, th = tiles[0].size; rows = (len(tiles) + cols - 1) // cols; gap = 12
    out = Image.new("RGB", (tw * cols + gap * (cols - 1), th * rows + gap * (rows - 1)), (8, 8, 10))
    for i, t in enumerate(tiles): out.paste(t, ((i % cols) * (tw + gap), (i // cols) * (th + gap)))
    os.makedirs(OUT, exist_ok=True)
    out.save(os.path.join(OUT, name), optimize=True); print(name, out.size)

sheet([tile(SRC, "Scanlines off"), tile(run("25"), "Scanlines on: 25%, 1 in 2 (default)")], 1, "scanline-off-on.png")
sheet([tile(run(s), s + "%") for s in ("12.5", "25", "50", "100")], 2, "scanline-strengths.png")
CL = (126, 150, 222, 214)                               # the sword, the arm, the crate's corner
sheet([tile(run("50", n), "1 in %d" % n, 6, CL) for n in (2, 3, 4)], 3, "scanline-spacing.png")
sheet([tile(SRC, "Off", 6, CL), tile(run("25"), "25%", 6, CL), tile(run("50"), "50%", 6, CL)], 3, "scanline-edges.png")
lab = run("25", labels=("SCANLINES 25%  1 IN 2  MODE 1", "COST 2.7ms  GE 0%"))
sheet([tile(lab, "The label after a change (about 3 seconds)")], 1, "scanline-label.png")
busy = run("25", labels=("SCANLINES 25%  1 IN 2  MODE 1", "COST 3.1ms  GE 40%  WAIT 0.6ms"))
sheet([tile(lab, "On", 4, (190, 0, 480, 34)), tile(busy, "On, graphics chip busy", 4, (190, 0, 480, 34)), tile(run(None, labels=("SCANLINES OFF FOR THIS GAME",)), "Off", 4, (190, 0, 480, 34))], 1, "scanline-label-closeup.png")
