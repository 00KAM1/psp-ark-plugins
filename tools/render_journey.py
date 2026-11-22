#!/usr/bin/env python3
"""Draws the journey log pictures for the docs: same coordinates, colours, wrapping
and paging as checkpoint/src/ui.c and checkpoint/src/journey.c. Stand-in game, invented entries.
Usage: python3 tools/render_journey.py"""
import os, sys; sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import render_screens as R
from PIL import Image
PAPER, RULE, INK, RED, FADE = (236,224,196), (208,194,166), (52,38,30), (150,48,34), (140,124,100)
COVER, COVER2, EDGE, SH1, SH2, SPINE = (74,40,24), (104,60,34), (214,200,172), (222,209,181), (200,186,158), (48,26,16)
DX, DY, DW, DH, PW, PH = 12, 8, 456, 244, 216, 228
LX, RX, PY = DX + 8, DX + 8 + PW + 8, DY + 8
COLS, PITCH, TOP, ROWS = 24, 11, 14, 18
MON = "Jan Feb Mar Apr May Jun Jul Aug Sep Oct Nov Dec".split(); DAY = "Sun Mon Tue Wed Thu Fri Sat".split()
def wrap(t):
    if len(t) <= COLS: return [t]
    cut = COLS
    while cut > 0 and t[cut] != ' ': cut -= 1
    if cut == 0: cut = COLS
    rest = t[cut:].lstrip(' ')[:COLS]
    return [t[:cut]] + ([rest] if rest else [])
def weekday(y, m, d):
    t = [0,3,2,5,0,3,5,1,4,6,2,4]
    if m < 3: y -= 1
    return (y + y//4 - y//100 + y//400 + t[m-1] + d) % 7
def paginate(es):
    first, used = [], ROWS + 1
    for i, e in enumerate(es):
        need = 2 + len(wrap(e[2]))
        if used + need > ROWS: first.append(i); used = 0
        used += need + 1
    return first
def line(im, right, k, s, fg=INK, centre=False):
    s = s[:COLS]; x = (RX if right else LX) + 12
    if centre: x += (COLS * 8 - len(s) * 8) // 2
    R.text(im, x, PY + TOP + k * PITCH, s, fg, PAPER)
def book(im):
    R.rect(im, DX, DY, DW, DH, COVER); R.rect(im, DX+2, DY+2, DW-4, DH-4, COVER2); R.rect(im, DX+4, DY+4, DW-8, DH-8, COVER)
    for x in (LX, RX):
        R.rect(im, x-2, PY+2, PW+4, PH, EDGE); R.rect(im, x, PY, PW, PH, PAPER)
        for k in range(ROWS): R.rect(im, x+8, PY+TOP+k*PITCH+9, PW-16, 1, RULE)
    R.rect(im, LX+PW-6, PY, 3, PH, SH1); R.rect(im, LX+PW-3, PY, 3, PH, SH2)
    R.rect(im, RX, PY, 3, PH, SH2); R.rect(im, RX+3, PY, 3, PH, SH1)
    R.rect(im, LX+PW, PY-2, 8, PH+4, SPINE); R.rect(im, RX+PW-30, PY-4, 6, 40, RED)
def page(im, right, es, first, p):
    if p < 0 or p >= len(first): return
    end = first[p+1] if p+1 < len(first) else len(es); k = 0
    for st, secs, text in es[first[p]:end]:
        y, m, d = int(st[:4]), int(st[4:6]), int(st[6:8])
        line(im, right, k, "%s %s %d   %s:%s" % (DAY[weekday(y,m,d)], MON[m-1], d, st[9:11], st[11:13]), RED); k += 1
        for w in wrap(text): line(im, right, k, w); k += 1
        line(im, right, k, "%dh %02dm" % (secs//3600, secs//60%60), FADE); k += 2
    s = "- %d -" % (p+1); R.text(im, (RX if right else LX) + (PW - len(s)*8)//2, PY+PH-12, s, FADE, PAPER)
def spread(bg, title, disc, es, total_secs, sp):
    im = bg.copy(); book(im); first = paginate(es); nsp = (len(first) + 2) // 2
    if sp is None: sp = nsp - 1
    if sp == 0:
        line(im, 0, 3, "Journey log", RED, True); k = 6
        if title:
            for w in wrap(title): line(im, 0, k, w, INK, True); k += 1
            k += 1
        line(im, 0, k, disc, FADE, True)
        if es:
            b = es[0][0]; line(im, 0, 12, "Begun %s %d %s" % (MON[int(b[4:6])-1], int(b[6:8]), b[:4]), INK, True)
        line(im, 0, 13, "%d entr%s" % (len(es), "y" if len(es) == 1 else "ies"), INK, True)
        line(im, 0, 14, "%dh %02dm played" % (total_secs//3600, total_secs//60%60), INK, True)
        if first: page(im, 1, es, first, 0)
        else:
            line(im, 1, 3, "No entries yet."); line(im, 1, 5, "A line is written when", FADE); line(im, 1, 6, "the game saves somewhere", FADE); line(im, 1, 7, "new.", FADE)
    else:
        page(im, 0, es, first, sp*2-1); page(im, 1, es, first, sp*2)
    R.rect(im, 0, 256, 480, 16, R.C_BG); keys = "L/R turn page    O close" if nsp > 1 else "O close"
    R.text(im, (480 - len(keys)*8)//2, 260, keys, R.C_WHITE, R.C_BG)
    return im
def menu_hint(im):
    R.text(im, R.BX + 4, R.BY + R.BH - 34 + 11, "SELECT journey log", R.C_GREY, R.C_BG); return im
def sheet(ims, name):
    S, pad = 2, 12
    out = Image.new("RGB", (480*S + 2*pad, len(ims)*272*S + (len(ims)+1)*pad), (12, 12, 16))
    for i, im in enumerate(ims): out.paste(im.resize((480*S, 272*S), Image.NEAREST), (pad, pad + i*(272*S + pad)))
    out.save(name); print(name, out.size)
if __name__ == "__main__":
    H = lambda h, m: h*3600 + m*60
    title, disc = R.GAMES["space"][0], R.GAMES["space"][1].replace("-", "")
    es = [("20261114_192200", H(0,25), "Sector 1: Outer Ring"), ("20261114_204100", H(1,40), "Sector 1: Relay Station"),
          ("20261115_210500", H(3,10), "Sector 2: The Gate"), ("20261116_223000", H(4,5), "Sector 2: Drift Yard"),
          ("20261117_201200", H(4,50), "Sector 3: Glass Moon"), ("20261117_214800", H(6,15), "Sector 3: Boss"),
          ("20261118_230500", H(7,20), "Sector 4: Ember Belt Outer Approach"), ("20261119_205800", H(8,5), "Sector 4: Ember Belt"),
          ("20261119_214300", H(8,50), "Sector 5: Quiet Harbour"), ("20261120_183000", H(9,40), "Sector 5: Last Light")]
    bg = R.game("space", 2)
    R.save(spread(bg, title, disc, es, H(9,40), 0), "journey-log-first-pages.png")
    R.save(spread(bg, title, disc, es, H(9,40), None), "journey-log-latest-pages.png")
