#!/usr/bin/env python3
"""Draws Emblem's pictures in docs/images.

The pop-up card and the viewer are drawn by the plugin's own code (src/draw.c),
and the viewer reads tools/demo-game.txt with the plugin's own engine (src/em.c).
The game behind them is a stand-in scene from tools/render_screens.py and the
achievements are made up; neither is a real game. Run from the emblem folder:

  gcc -O2 -std=gnu99 -Itests/shim -include tests/shim/psptypes.h -o /tmp/emblem_card tools/render_tool.c src/draw.c
  gcc -O2 -std=gnu99 -Itests/shim -include tests/shim/psptypes.h -o /tmp/emblem_view tools/render_view.c src/draw.c src/em.c
  python3 tools/render_docs.py /tmp/emblem_card /tmp/emblem_view          (needs Pillow)
"""
import importlib.util, os, subprocess, sys, tempfile
from PIL import Image
HERE = os.path.dirname(os.path.abspath(__file__))
TOP = os.path.join(HERE, "..", "..")
spec = importlib.util.spec_from_file_location("render_screens", os.path.join(TOP, "tools", "render_screens.py"))
rs = importlib.util.module_from_spec(spec); spec.loader.exec_module(rs)
CARD, VIEW = sys.argv[1], sys.argv[2]
FONT = os.path.join(TOP, "tools", "msx.bin")
GAME = os.path.join(HERE, "demo-game.txt")
TMP = tempfile.mkdtemp()
BUILD = "1.0  Nov 22 2026 02:55:47"
EARNED = ["1=2026-11-20 19:12", "2=2026-11-20 20:03", "3=2026-11-21 18:40", "5=2026-11-22 12:31"]

def through(tool_args, scene):
    a, b = os.path.join(TMP, "in.rgb"), os.path.join(TMP, "out.rgb")
    open(a, "wb").write(scene.convert("RGB").tobytes())
    subprocess.check_call([x.replace("@IN", a).replace("@OUT", b) for x in tool_args])
    return Image.frombytes("RGB", (480, 272), open(b, "rb").read())
def card(scene, head, title, foot):
    return through([CARD, "@IN", "@OUT", "0", FONT, "card", "2000", head, title, foot], scene)
def view(scene, page, first, cursor):
    return through([VIEW, GAME, FONT, "@IN", "@OUT", str(page), str(first), str(cursor), "games/DEMO00101.txt", BUILD] + EARNED, scene)

bg = rs.game("field", 7)
rs.save(card(bg, "Achievement unlocked", "Keeper of the Marsh", "10 pts   5 of 12"), "emblem-unlock.png")
rs.save(card(rs.game("field", 3), "Already earned", "3 found in this save", "3 of 12"), "emblem-already-earned.png")
rs.save(view(bg, 0, 0, 3), "emblem-viewer.png")
rs.save(view(bg, 1, 0, 0), "emblem-viewer-values.png")
