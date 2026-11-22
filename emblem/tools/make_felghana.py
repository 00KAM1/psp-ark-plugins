#!/usr/bin/env python3
"""Writes games/ULUS10558.txt (Ys: The Oath in Felghana) for Emblem.

The address lists and titles come from Achieve 0.2's data (tests/reference/
felghana_data.inc), itself generated from RetroAchievements' list and public
code notes for the game; the rules are Achieve 0.2's conditions written in
Emblem's file format. Run from the project folder:
    python3 tools/make_felghana.py > games/ULUS10558.txt
"""
import re, sys
src = open("tests/reference/felghana_data.inc").read()
titles = {int(i): (t, int(p)) for i, t, p in re.findall(r'\{ (\d+), "([^"]+)", (\d+) \}', src)}
def lst(name):
    body = re.search(r"static const u32 %s\[\d+\] = \{([^}]*)\}" % name, src).group(1)
    return ["%08X" % int(a.strip().rstrip("u"), 16) for a in body.split(",") if a.strip()]
LEFT_OUT = {4, 5, 6, 13, 38, 95}
def best(d, f): return "%08X" % (0x08BBA6A0 + 0xC0 * d + 0xC * f)

out = []
w = out.append
w("# Emblem achievements for Ys: The Oath in Felghana (USA), ULUS-10558.")
w("# Titles and points: RetroAchievements' list for the game (set 3132).")
w("# Addresses: RetroAchievements' public code notes (RA address + 0x08000000).")
w("# Not here (no address in the notes): The Black-Hearted White Knight, The Fire Eater,")
w("# Lord of the Blazing Prison, Savior of Swine, Never Smelled the Roses, Sightseeing in Felghana.")
w("")
w("game ULUS10558 Ys: The Oath in Felghana")
w("")
w("# ---- values ----")
for n, d in [("mode", "u16 08BC4CCC       # 0 main game, 1 Time Attack, 2 Museum"),
             ("level", "u32 08BB9B30"), ("gold", "u32 08BB9B2C"),
             ("elixir", "s32 08BB9E38       # Katol Elixirs carried; -1 when none"),
             ("maxhp", "float 08BB9AFC"), ("hp", "float 08BB9B00"),
             ("diff", "u32 08BB9EF4       # 0 Normal, 1 Hard, 2 Nightmare (Inferno: + infernof)"),
             ("easy", "u32 08BB9EF0       # set on Very Easy and Easy"),
             ("lowered", "u32 08BBA468       # difficulty lowered after retries"),
             ("infernof", "u32 08BB9FDC"), ("combo", "s32 08BBA5DC"),
             ("ta_state", "u16 08BC4CCE       # Time Attack: 0x28 menu, 0x29 fight, 0x2a after"),
             ("fight", "u16 08BC4CD8       # 0 Boss Rush, 1 Dularn ... 15 Galbalan"),
             ("ta_sel", "u8 08BC4E36        # difficulty picked: 0 Very Easy ... 4 Nightmare"),
             ("lost", "u8 089FC932        # 1: lost the fight"),
             ("ta_hp", "float 089F8D48")]:
    w("%-9s= %s" % (n, d))
w("")
w("# ---- when Emblem looks: main game or Time Attack, with sane values ----")
w("main     = mode == 0 and level 1..99 and maxhp 1..9999 and diff 0..2 and gold 0..9999999 and elixir -1..99")
w("ta       = mode == 1 and ta_state 0x28..0x2a and fight 0..32")
w("ready    = main or ta")
w("")
w("# Bosses, the bestiary and the combos count on any difficulty. Hard, Nightmare and")
w("# Inferno achievements need that difficulty, picked at the start and never lowered.")
w("strict    = easy off and lowered off")
w("normal    = 1 == 1")
w("hard      = strict and diff >= 1")
w("nightmare = strict and diff == 2 and infernof off")
w("inferno   = strict and diff == 2 and infernof set")
w("")
w("# ---- story, bosses and items ----")
flags = [("dularn1", "08BBA020", "Fight Dularn"), ("ellefale", "08BBA028", "Win Ellefale fight"),
         ("istersiva", "08BBA060", ""), ("succubi", "08BBA068", "After Succubi fight"),
         ("gildias", "08BBA07C", ""), ("faleon", "08BBA098", ""), ("schultiger", "08BBA09C", ""),
         ("zirduros", "08BBA0B0", ""), ("chester2", "08BBA0BC", ""), ("dularn2", "08BBA0C8", ""),
         ("garland", "08BBA0CC", ""), ("galbalan", "08BBA0E4", ""),
         ("anya", "08BBA1B4", "Show Anya the 3 Raval Ore from well"),
         ("antonio", "08BBA168", "4: 10000 gold and the Jade Ring from Antonio"),
         ("cynthia", "08BBA1A4", "Show Cynthia the Jade Ring"), ("fiona", "08BBA458", "Talk to Fiona (get amulet)"),
         ("nightfire", "08BB9D84", ""), ("stoneshoes", "08BB9D8C", ""), ("spiritcape", "08BB9D90", ""),
         ("bravesword", "08BB9D50", ""), ("tablet", "08BB9DFC", "Mission Tablet"),
         ("moonstar", "08BB9E0C", ""), ("sunset", "08BB9E10", ""), ("darkness", "08BB9E14", ""),
         ("light", "08BB9E18", ""), ("talisman", "08BB9E40", ""), ("augite", "08BB9E44", ""),
         ("lochus", "08BBA11C", "Raval Ore given: 7"), ("well", "08BBA120", "3"), ("edgar", "08BBA140", "20"),
         ("joel", "08BBA45C", "12"), ("harold", "08BBA460", "100")]
for n, a, c in flags:
    t = "s32" if n == "antonio" else "u32"
    w(("%-11s= %s %s" % (n, t, a)) + ("    # " + c if c else ""))
w("elena_back = u32 08BBA0F4 bit 0")
w("show level diff easy lowered dularn1 ellefale mode gold elixir")
w("")
w("# ---- lists ----")
for name, ref, typ in [("chests1", "FG_CHESTS_I", "u32"), ("chests2", "FG_CHESTS_II", "u32"), ("chests3", "FG_CHESTS_III", "u32"),
                       ("chests4", "FG_CHESTS_IV", "u32"), ("chests5", "FG_CHESTS_V", "u32"), ("urns", "FG_URNS", "u32"),
                       ("chars", "FG_CHARS", "u8 bit 0"), ("beasts", "FG_MONSTERS", "u8 bit 0")]:
    w("%-8s= list %s %s" % (name, typ, " ".join(lst(ref))))
w("")
rules = {
    1: "normal and dularn1 set", 2: "normal and ellefale set", 3: "fiona set", 7: "nightfire set",
    8: "normal and augite set", 9: "normal and istersiva set", 10: "anya set", 11: "normal and elena_back set",
    12: "normal and succubi set", 14: "antonio >= 4", 15: "cynthia set", 16: "talisman set",
    17: "just gildias set and normal and spiritcape set",
    18: "normal and gildias set and stoneshoes off", 19: "normal and gildias set",
    20: "normal and faleon set", 21: "normal and schultiger set", 22: "normal and zirduros set",
    23: "normal and chester2 set", 24: "normal and dularn2 set", 25: "normal and garland set",
    26: "normal and galbalan set", 27: "hard and galbalan set",
    28: "nightmare and moonstar set", 29: "nightmare and sunset set", 30: "nightmare and darkness set",
    31: "nightmare and light set", 32: "nightmare and galbalan set",
    33: "inferno and moonstar set", 34: "inferno and sunset set", 35: "inferno and darkness set",
    36: "inferno and light set", 37: "inferno and galbalan set",
    39: "all chests1", 40: "all chests2", 41: "all chests3", 42: "all chests4", 43: "all chests5",
    44: "count urns >= 10", 45: "count urns >= 25", 46: "all urns",
    47: "count chars >= 16", 48: "all chars",
    49: "normal and count beasts >= 16", 50: "normal and count beasts >= 32",
    51: "normal and count beasts >= 64", 52: "normal and all beasts",
    53: "normal and combo >= 75 and moonstar off", 54: "normal and combo >= 125 and moonstar set and sunset off",
    55: "normal and combo >= 250 and sunset set and darkness off", 56: "normal and combo >= 500 and bravesword set",
    57: "level >= 10", 58: "level >= 30", 59: "level >= 60",
    72: "(harold set or (lochus set and well set and edgar set and joel set)) and tablet off",
    73: "gold drops 999999 within 3s and elixir rises within 3s",
}
for t in range(1, 7):                                   # NG / NG+ upgrades: all three pieces of tier t
    a = ["%08X" % (0x08BB9AB4 + 4 * i) for i in (t - 1, t + 5, t + 11)]
    for lv, base in ((2, 59), (8, 65)):                 # shown in the game as one more: level 3 and 9
        rules[base + t] = " and ".join("s32 %s >= %d" % (x, lv) for x in a)

w("# ---- main game: number | title | points | rule ----")
w("when main")
for i in sorted(rules):
    t, p = titles[i]
    w("%d | %s | %d | %s" % (i, t, p, rules[i]))
w("")
w("# ---- Time Attack ----")
w("when ta")
w("fighting = ta_state == 0x29")
w("won      = ta_state == 0x2a and was ta_state == 0x29")
w("hurt     = fighting and (ta_hp drops or hp drops or lost == 1)")
w("fell     = fighting and lost == 1")
ta = {1: 82, 2: 83, 3: 84, 5: 85, 6: 86, 7: 87, 9: 88, 10: 89, 11: 90, 12: 91, 13: 92, 14: 93, 15: 94}
for f in sorted(ta):
    w("rec%d_hi  = list u32 %s" % (f, " ".join(best(d, f) for d in range(2, 6))))
    w("rec%d_all = list u32 %s" % (f, " ".join(best(d, f) for d in range(6))))
for d in range(6):
    w("rush%d    = u16 08BBAB48 bit %d" % (d, 6 + d))
    w("rushrec%d = u32 %s" % (d, best(d, 0)))
w("berhardt = u32 08BBAB20")
w("pikkard  = u32 08BBAB2C")
w("rush_go  = fighting and was ta_state == 0x28 and fight == 0")
w("to_menu  = ta_state == 0x28 and not was ta_state == 0x28")
w("")
w("# Boss Rush: from leaving the menu until its 'beaten' bit or its record moves")
for d in range(6):
    i = 74 + d; t, p = titles[i]
    rule = "run from rush_go to moved rush%d or (rushrec%d set and moved rushrec%d)" % (d, d, d)
    if d <= 3: rule += " unless fell"
    rule += " cancel to_menu"
    w("%d | %s | %d | %s" % (i, t, p, rule))
for i, nm in ((80, "berhardt"), (81, "pikkard")):
    t, p = titles[i]
    w("%d | %s | %d | run from fighting to won and %s set and moved %s unless fell" % (i, t, p, nm, nm))
w("# One fight on Normal or harder without damage: the record that moved says the difficulty, else the pick")
for f in sorted(ta):
    i = ta[f]; t, p = titles[i]
    w("%d | %s | %d | run from fighting and fight == %d to won and (moved rec%d_hi or (not moved rec%d_all and ta_sel >= 2)) unless hurt grace 0.5s"
      % (i, t, p, f, f, f))
assert not (set(rules) | set(ta.values()) | set(range(74, 82))) & LEFT_OUT
print("\n".join(out))
n = len(rules) + len(ta) + 8
print("wrote %d achievements" % n, file=sys.stderr)
