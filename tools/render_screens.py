#!/usr/bin/env python3
"""Draws the documentation screenshots for Sidequest and Checkpoint (Emblem's and
Scanline's have scripts of their own, which borrow the scenes from this one).

Every screen is laid out with the same coordinates, colours, text and 8x8 font
as the plugins' own drawing code (sidequest/src/ui.c, checkpoint/src/ui.c).
The games behind the menus are stand-in scenes drawn by this script; they are
not real games. Output: docs/images/*.png at twice the PSP's 480x272.

Usage: python3 tools/render_screens.py        (needs Pillow)
"""
import os, random
from PIL import Image, ImageDraw, ImageEnhance

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "..", "docs", "images")
FONT = open(os.path.join(HERE, "msx.bin"), "rb").read()
W, H = 480, 272

# plugin colours (blit.h), stored there as 0x00BBGGRR
def col(c): return (c & 0xFF, (c >> 8) & 0xFF, (c >> 16) & 0xFF)
C_WHITE, C_GREY, C_BG, C_HI = col(0xFFFFFF), col(0xA0A0A0), col(0x301A10), col(0x805020)
C_OK, C_WARN, C_ERR, C_DIM = col(0x60E060), col(0x40C0FF), col(0x4040FF), col(0x181008)
BX, BY, BW, BH = 24, 28, 432, 216

def rect(im, x, y, w, h, c): ImageDraw.Draw(im).rectangle((x, y, x + w - 1, y + h - 1), fill=c)
def text(im, x, y, s, fg, bg):
    px = im.load()
    for ch in s.upper():
        g = FONT[ord(ch) * 8: ord(ch) * 8 + 8]
        for r in range(8):
            for c in range(8):
                if 0 <= x + c < W and 0 <= y + r < H: px[x + c, y + r] = fg if g[r] & (0x80 >> c) else bg
        x += 8

# ---- stand-in game scenes (original, drawn here) ---------------------------
def scene_field(seed):
    """Top-down adventure: grass, a path, trees, a pond, a small hero and a HUD."""
    rnd = random.Random(seed); im = Image.new("RGB", (W, H)); d = ImageDraw.Draw(im)
    for ty in range(0, H, 16):
        for tx in range(0, W, 16):
            g = rnd.choice([(58, 132, 62), (64, 140, 66), (54, 124, 60), (70, 148, 70)])
            d.rectangle((tx, ty, tx + 15, ty + 15), fill=g)
            if rnd.random() < .25: d.point((tx + rnd.randrange(16), ty + rnd.randrange(16)), fill=(96, 176, 84))
    py = 96 + rnd.randrange(80)
    for x in range(0, W, 4):
        y = py + int(18 * __import__("math").sin((x + seed * 40) / 70))
        d.rectangle((x, y, x + 3, y + 30), fill=(196, 168, 112)); d.rectangle((x, y + 30, x + 3, y + 32), fill=(150, 124, 80))
    px_, py_ = 300 + rnd.randrange(100), 30 + rnd.randrange(30)
    d.ellipse((px_, py_, px_ + 110, py_ + 54), fill=(52, 120, 196)); d.ellipse((px_ + 10, py_ + 8, px_ + 70, py_ + 26), fill=(92, 160, 224))
    for _ in range(14):
        x, y = rnd.randrange(W - 24), rnd.randrange(H - 60)
        if abs(y - py) < 44 or (px_ - 24 < x < px_ + 110 and py_ - 30 < y < py_ + 54): continue
        d.rectangle((x + 9, y + 20, x + 14, y + 30), fill=(104, 72, 44))
        d.ellipse((x, y, x + 23, y + 23), fill=(28, 92, 48)); d.ellipse((x + 4, y + 3, x + 15, y + 12), fill=(48, 124, 62))
    hx, hy = 120 + rnd.randrange(160), py + 4
    d.rectangle((hx + 2, hy + 8, hx + 9, hy + 17), fill=(196, 60, 52)); d.rectangle((hx + 3, hy, hx + 8, hy + 7), fill=(240, 204, 160))
    d.rectangle((hx + 2, hy - 2, hx + 9, hy + 1), fill=(92, 60, 36)); d.rectangle((hx + 10, hy + 6, hx + 11, hy + 16), fill=(220, 220, 232))
    d.rectangle((hx + 2, hy + 18, hx + 4, hy + 20), fill=(60, 44, 36)); d.rectangle((hx + 7, hy + 18, hx + 9, hy + 20), fill=(60, 44, 36))
    d.rectangle((8, 8, 150, 24), fill=(20, 24, 40)); d.rectangle((12, 12, 12 + 60 + rnd.randrange(60), 20), fill=(220, 64, 64))
    for i in range(3): d.ellipse((392 + i * 26, 8, 408 + i * 26, 24), fill=(248, 208, 72), outline=(120, 88, 20))
    return im

def scene_road(seed):
    """Pseudo-3D racer: sky, hills, a striped road, a car and a speed read-out."""
    rnd = random.Random(seed); im = Image.new("RGB", (W, H)); d = ImageDraw.Draw(im); hz = 118
    for y in range(hz): d.line((0, y, W, y), fill=(250 - y // 2, 150 + y // 3, 96 + y))
    d.ellipse((330 - seed * 30 % 200, 30, 390 - seed * 30 % 200, 90), fill=(255, 236, 170))
    for i in range(7):
        x = i * 90 - 40 + rnd.randrange(30); h = 24 + rnd.randrange(34)
        d.polygon([(x, hz), (x + 70, hz - h), (x + 150, hz)], fill=(86 + i * 4, 70, 120))
    bend = (seed % 3 - 1) * 60
    for y in range(hz, H):
        t = (y - hz) / (H - hz); cx = W // 2 + int(bend * (1 - t) ** 2); half = int(18 + 250 * t)
        band = int((1 / (t + .06)) * 3 + seed) % 2
        d.line((0, y, W, y), fill=(52, 150, 70) if band else (44, 132, 62))
        d.line((cx - half - 8, y, cx + half + 8, y), fill=(236, 236, 236) if band else (208, 56, 56))
        d.line((cx - half, y, cx + half, y), fill=(84, 84, 92) if band else (76, 76, 84))
        if band: d.line((cx - max(1, half // 30), y, cx + max(1, half // 30), y), fill=(240, 240, 240))
    cx = W // 2 + (seed % 5 - 2) * 14
    d.rectangle((cx - 34, 214, cx + 34, 238), fill=(28, 84, 200)); d.rectangle((cx - 24, 200, cx + 24, 216), fill=(20, 60, 150))
    d.rectangle((cx - 20, 203, cx + 20, 212), fill=(150, 200, 236)); d.rectangle((cx - 40, 232, cx - 26, 246), fill=(16, 16, 20)); d.rectangle((cx + 26, 232, cx + 40, 246), fill=(16, 16, 20))
    d.rectangle((cx - 30, 222, cx - 20, 228), fill=(250, 80, 60)); d.rectangle((cx + 20, 222, cx + 30, 228), fill=(250, 80, 60))
    rect(im, 8, 8, 96, 20, (16, 16, 28)); text(im, 12, 14, "LAP %d/3" % (seed % 3 + 1), (250, 230, 120), (16, 16, 28))
    rect(im, 368, 8, 104, 20, (16, 16, 28)); text(im, 372, 14, "%3d KM/H" % (140 + seed * 17 % 90), (250, 250, 250), (16, 16, 28))
    return im

def scene_space(seed):
    """Vertical shooter: stars, a planet, the player's ship, enemies and a score."""
    rnd = random.Random(seed); im = Image.new("RGB", (W, H), (8, 8, 24)); d = ImageDraw.Draw(im)
    for _ in range(170):
        v = rnd.choice([90, 140, 200, 255]); d.point((rnd.randrange(W), rnd.randrange(H)), fill=(v, v, min(255, v + 30)))
    x0 = 300 + seed * 37 % 120
    d.ellipse((x0, 140, x0 + 150, 290), fill=(150, 84, 170)); d.ellipse((x0 + 20, 156, x0 + 100, 220), fill=(180, 110, 196))
    for _ in range(6):
        x, y = 40 + rnd.randrange(380), 30 + rnd.randrange(110)
        d.polygon([(x, y), (x + 20, y), (x + 10, y + 16)], fill=(230, 80, 70)); d.rectangle((x + 8, y - 4, x + 12, y), fill=(250, 190, 70))
    sx = 150 + seed * 53 % 180
    d.polygon([(sx, 232), (sx + 14, 200), (sx + 28, 232)], fill=(90, 210, 230)); d.rectangle((sx + 11, 214, sx + 17, 226), fill=(240, 250, 255))
    d.polygon([(sx + 8, 232), (sx + 14, 246), (sx + 20, 232)], fill=(255, 170, 50))
    for i in range(4): d.rectangle((sx + 13, 180 - i * 28, sx + 15, 190 - i * 28), fill=(255, 255, 140))
    text(im, 8, 8, "SCORE %06d" % (12300 + seed * 4170), (250, 250, 250), (8, 8, 24)); text(im, 392, 8, "SHIPS x%d" % (3 - seed % 2), (120, 230, 140), (8, 8, 24))
    return im

GAMES = {  # invented titles and IDs
    "field": ("Lantern Hollow", "DEMO-00101", scene_field),
    "road":  ("Gridline Rally", "DEMO-00202", scene_road),
    "space": ("Starfold",       "DEMO-00303", scene_space),
}
def game(key, seed): return GAMES[key][2](seed)

def thumb(key, seed):
    """Slot picture as the plugin stores it: 240x136, 2x2 average, RGB565."""
    t = game(key, seed).resize((240, 136), Image.BOX); px = t.load()
    for y in range(136):
        for x in range(240):
            r, g, b = px[x, y]; px[x, y] = ((r >> 3) << 3 | r >> 5, (g >> 2) << 2 | g >> 6, (b >> 3) << 3 | b >> 5)
    return t

# ---- Sidequest (sidequest/src/ui.c) ----------------------------------------
LIST_W = 168; TH_X, TH_Y = BX + LIST_W + 12, BY + 34
def sq_row_y(k): return BY + 48 if k == 0 else BY + 82 + (k - 1) * 14
def sq_menu(bg, title, slots, sel, own=True, page=None, need=None, status=None, pic=None):
    """slots: six entries, each None or "MM-DD HH:MM". pic: (game key, seed) of the selected slot."""
    im = bg.copy()
    rect(im, BX, BY, BW, BH, C_BG); rect(im, BX, BY, BW, 12, C_HI)
    text(im, BX + 4, BY + 2, "Sidequest 1.0", C_WHITE, C_HI)
    if need and own:
        s = "needs ~%d of %d MB" % need; text(im, BX + BW - 4 - 8 * len(s), BY + 2, s, C_WHITE, C_HI)
    text(im, BX + 4, BY + 18, title[:40], C_WHITE if own else C_WARN, C_BG)
    if page: s = "L/R %d/%d" % page; text(im, BX + BW - 4 - 8 * len(s), BY + 18, s, C_GREY, C_BG)
    text(im, BX + 6, BY + 36, "Suspend", C_GREY, C_BG)
    rect(im, BX + 4, BY + 62, LIST_W - 4, 1, C_HI)
    text(im, BX + 6, BY + 70, "States", C_GREY, C_BG)
    for k in range(6):
        y, bgc, when = sq_row_y(k), (C_HI if k == sel else C_BG), slots[k]
        rect(im, BX + 2, y - 2, LIST_W, 12, bgc)
        if k == 0: text(im, BX + 6, y, "   " + (when or "(none)"), C_OK if when else C_GREY, bgc)
        else:      text(im, BX + 6, y, "%d  %s" % (k, when or "(empty)"), C_WHITE if when else C_GREY, bgc)
    rect(im, TH_X - 1, TH_Y - 1, 242, 138, C_HI)
    if slots[sel] and pic: im.paste(thumb(*pic), (TH_X, TH_Y))
    else:
        rect(im, TH_X, TH_Y, 240, 136, C_DIM); text(im, TH_X + 120 - 20, TH_Y + 64, "Empty", C_GREY, C_DIM)
    fy = BY + BH - 34
    if not own:        keys = "[] load   O close" if sel == 0 else "[] load   /\\ delete   O close"
    elif sel == 0:     keys = "[] load   O close"
    elif slots[sel]:   keys = "X save   [] load   /\\ delete   O close"
    else:              keys = "X save   O close"
    text(im, BX + 4, fy, keys, C_WHITE, C_BG)
    if status:      text(im, BX + 4, fy + 22, status[0][:52], status[1], C_BG)
    elif not own:   text(im, BX + 4, fy + 22, "Another game: loading it switches games", C_GREY, C_BG)
    elif sel == 0:  text(im, BX + 4, fy + 22, "Suspend is written by Suspend and Quit only", C_GREY, C_BG)
    return im
def message(im, l1, l2, c):
    rect(im, BX, BY + 70, BW, 60, C_BG); text(im, BX + 8, BY + 84, l1, c, C_BG)
    if l2: text(im, BX + 8, BY + 100, l2, C_GREY, C_BG)
    return im
def sq_prompt(bg, title, line, pic):
    im = bg.copy(); X, Y, Wd, Hd = 8, 184, 464, 80
    rect(im, X, Y, Wd, Hd, C_BG); rect(im, X, Y, Wd, 2, C_HI)
    im.paste(thumb(*pic).resize((120, 68), Image.NEAREST), (X + 6, Y + 7)); tx = X + 8 + 128
    text(im, tx, Y + 10, "Continue where you left off?", C_WHITE, C_BG)
    text(im, tx, Y + 26, title[:38], C_GREY, C_BG); text(im, tx, Y + 40, line, C_GREY, C_BG)
    text(im, tx, Y + 58, "X continue   O start the game", C_OK, C_BG)
    return im

# ---- Checkpoint (checkpoint/src/ui.c) --------------------------------------
def cp_frame(im, right):
    rect(im, BX, BY, BW, BH, C_BG); rect(im, BX, BY, BW, 12, C_HI)
    text(im, BX + 4, BY + 2, "Checkpoint 1.0", C_WHITE, C_HI); text(im, BX + BW - 4 - 8 * len(right), BY + 2, right, C_WHITE, C_HI)
def cp_footer(im, keys, status=None):
    fy = BY + BH - 34; text(im, BX + 4, fy, keys, C_WHITE, C_BG)
    text(im, BX + 4, fy + 11, "SELECT journey log", C_GREY, C_BG)
    if status: text(im, BX + 4, fy + 22, status[0][:52], status[1], C_BG)
def cp_row(im, k, selected, s, fg):
    y, bgc = BY + 34 + k * 14, (C_HI if selected else C_BG)
    rect(im, BX + 2, y - 2, BW - 4, 12, bgc); text(im, BX + 6, y, s, fg, bgc)
def cp_saves(bg, disc, saves, sel, status=None):
    im = bg.copy(); cp_frame(im, disc); text(im, BX + 4, BY + 18, "Saves of this game", C_GREY, C_BG)
    for k, (name, n, title) in enumerate(saves): cp_row(im, k, k == sel, "%-24.24s %2d %.24s" % (name, n, title), C_WHITE)
    cp_footer(im, "X versions   [] back up now   O close", status); return im
def cp_versions(bg, disc, save, vers, sel, status=None):
    im = bg.copy(); cp_frame(im, disc); text(im, BX + 4, BY + 18, save, C_GREY, C_BG)
    for k, (stamp, pinned, detail) in enumerate(vers):
        s = "%2d %s %s-%s %s:%s %.33s" % (k + 1, "PIN" if pinned else "   ", stamp[4:6], stamp[6:8], stamp[9:11], stamp[11:13], detail)
        cp_row(im, k, k == sel, s, C_OK if pinned else C_WHITE)
    cp_footer(im, "X restore  /\\ delete  [] back up  START pin  O back", status); return im
def cp_quit(bg, disc, head, c, l2, l3, sidequest=True, emblem=True):
    """The button rows are only there for the plugins running in the game (checkpoint/src/ui.c, quit_screen)."""
    im = bg.copy(); X, Y, Wd, Hd = 56, 64, 368, 144
    if sidequest: Hd += 14; Y -= 7
    K1, K2 = X + 12, X + 12 + 17 * 8
    rect(im, X, Y, Wd, Hd, C_BG); rect(im, X, Y, Wd, 12, C_HI)
    text(im, X + 4, Y + 2, "Checkpoint", C_WHITE, C_HI); text(im, X + Wd - 4 - 8 * len(disc), Y + 2, disc, C_WHITE, C_HI)
    text(im, X + 12, Y + 26, "Quit the game?", C_WHITE, C_BG); rect(im, X + 12, Y + 46, 4, 44, c)
    text(im, X + 24, Y + 48, head, c, C_BG); text(im, X + 24, Y + 64, l2, C_GREY, C_BG); text(im, X + 24, Y + 80, l3, C_GREY, C_BG)
    y = Y + 112
    if sidequest:
        text(im, K1, y, "L  Sidequest", C_GREY, C_BG)
        if emblem: text(im, K2, y, "R  Emblem", C_GREY, C_BG)
        y += 14
    text(im, K1, y, "[] checkpoints", C_GREY, C_BG)
    if sidequest: text(im, K2, y, "/\\ suspend and quit", C_GREY, C_BG)
    elif emblem:  text(im, K2, y, "R  Emblem", C_GREY, C_BG)
    y += 14
    text(im, K1, y, "X  quit", C_WHITE, C_BG); text(im, K2, y, "O  back to game", C_WHITE, C_BG)
    return im

# ---- output ----------------------------------------------------------------
def psp_look(im):
    """Roughly how a PSP-3000 panel shows it: a little brighter and more vivid than raw values."""
    im = im.point(lambda v: int(255 * (v / 255) ** 0.86))
    return ImageEnhance.Color(im).enhance(1.12)
def save(im, name):
    os.makedirs(OUT, exist_ok=True)
    psp_look(im).resize((W * 2, H * 2), Image.NEAREST).save(os.path.join(OUT, name), optimize=True); print(name)

if __name__ == "__main__":
    field, road, space = GAMES["field"], GAMES["road"], GAMES["space"]
    slots = ["11-19 21:45", "11-19 20:10", "11-18 23:05", "11-16 18:32", None, None]
    bg = game("field", 7)
    save(sq_menu(bg, field[0], slots, 1, need=(16, 27), page=(1, 3), pic=("field", 3)), "sidequest-menu.png")
    save(sq_menu(bg, field[0], slots, 0, need=(16, 27), page=(1, 3), pic=("field", 7)), "sidequest-suspend-slot.png")
    save(sq_menu(bg, field[0], slots, 4, need=(16, 27), page=(1, 3)), "sidequest-empty-slot.png")
    other = ["11-17 22:10", "11-17 21:02", "11-15 19:40", None, None, None]
    save(sq_menu(bg, road[0], other, 1, own=False, page=(2, 3), pic=("road", 4)), "sidequest-other-game.png")
    m = sq_menu(bg, field[0], slots, 2, need=(16, 27), page=(1, 3), pic=("field", 5))
    save(message(m.copy(), "Overwrite this slot?", "X: yes     any other button: no", C_WARN), "sidequest-confirm.png")
    save(message(m.copy(), "Saving. The screen goes dark for a few seconds.", "If it stays dark, slide POWER up.", C_WARN), "sidequest-saving.png")
    save(sq_prompt(game("field", 1), field[0], "Suspended 11-19 21:45", ("field", 7)), "sidequest-continue-prompt.png")

    bg2 = game("space", 2); disc = space[1]; sv = disc.replace("-", "")
    saves = [(sv + "_SAVE01", 6, "Sector 4: Ember Belt"), (sv + "_SAVE02", 2, "Sector 1: Outer Ring"), (sv + "_SYS", 1, "System data")]
    save(cp_saves(bg2, disc, saves, 0), "checkpoint-saves.png")
    vers = [("20261119_214012", 0, "Sector 4: Ember Belt  02:41:10"), ("20261119_203355", 0, "Sector 4: Ember Belt  02:12:44"),
            ("20261118_230102", 1, "Sector 3: Boss  01:58:03"), ("20261118_221540", 0, "Sector 3: Glass Moon  01:40:27"),
            ("20261117_195208", 1, "Sector 2: The Gate  01:02:15"), ("20261116_183001", 0, "Sector 1: Outer Ring  00:21:36")]
    v = cp_versions(bg2, disc, saves[0][0], vers, 2)
    save(v, "checkpoint-versions.png")
    save(message(v.copy(), "Replace the game's current save with this version?", "X: yes     any other button: no", C_WARN), "checkpoint-restore-confirm.png")
    save(cp_versions(bg2, disc, saves[0][0], vers, 2, status=("Restored. Now load it from the game's load screen.", C_OK)), "checkpoint-restored.png")
    bg3 = game("road", 5); d3 = road[1]
    save(cp_quit(bg3, d3, "Saved 4 minutes ago", C_OK, "Cup 2: Harbour Loop  11-19 21:41", "9 checkpoints kept, 2 pinned"), "quit-screen-saved.png")
    save(cp_quit(bg3, d3, "Last saved 47 minutes ago", C_WARN, "Cup 2: Harbour Loop  11-19 20:58", "9 checkpoints kept, 2 pinned", emblem=False), "quit-screen-old-save.png")
    save(cp_quit(bg3, d3, "Not saved this session", C_ERR, "Last: Cup 1: Pine Ridge  11-18 22:30", "8 checkpoints kept, 2 pinned"), "quit-screen-not-saved.png")
    save(cp_quit(bg3, d3, "Saved 4 minutes ago", C_OK, "Cup 2: Harbour Loop  11-19 21:41", "9 checkpoints kept, 2 pinned", sidequest=False, emblem=False), "quit-screen-alone.png")
