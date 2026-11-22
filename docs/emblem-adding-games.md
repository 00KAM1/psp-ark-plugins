# Emblem: adding a game

Adding a game means writing **one text file**. The plugin is not rebuilt. Put the file in `ms0:/EMBLEM/games/` and Emblem picks it up the next time that game starts.

`emblem/games/ULUS10558.txt` (Ys: The Oath in Felghana) is the fullest example: plain flags, lists, things that happen over time, and Time Attack runs. `emblem/tools/demo-game.txt` is a short made-up one.

## Steps

1. **Game ID.** The file is `<ID>.txt`, with the ID as Emblem's log gives it in its first line (`game 'ULUS10558'`): no dash. `emblem/tools/game_ids.py` prints the ID of every ISO in a folder. A Japanese release has different addresses from the US one, and a fan translation can too, even with the same ID.
2. **The achievement list** (titles, descriptions, points): the game's page on [RetroAchievements](https://retroachievements.org).
3. **The memory map.** RetroAchievements' code notes for a game are public: `https://retroachievements.org/dorequest.php?r=codenotes2&g=<their game number>` returns them as JSON. **PSP address = their address + 0x08000000.** A note says the size (`[32-bit]`, `[8-bit]`, `[float]`) and what the bits mean.
4. **Match each achievement's description to notes.** Sort them into plain (a flag, a count, a level), over-time (something happens: a purchase, a boss falls while something else holds), runs (a challenge from A to B without C), and no note at all. Leave the last kind out of the file: the viewer's totals count only what the file offers.
5. **Write the file** in the format below.
6. **Check it on a PC** before it goes on the PSP. `emblem/tests/test_engine.c` loads a file with the real engine and prints every error with its line number; copy its "Felghana file" block for a new game. At the least: the file loads with no errors, and nothing unlocks from memory that is all zeros or all `0xFF` once `ready` holds.

## The file format
One thing per line. `#` starts a comment (also after a rule). Names: letters, digits, `_`; not a word Emblem uses.
```
game ULUS10558 Ys: The Oath in Felghana         # ID, then the title the viewer shows

level  = u32 08BB9B30                            # a name for a value: u8 / u16 / u32 / s32 / float ADDR [bit N]
normal = easy off and lowered off                # a name for a rule
urns   = list u32 08BBA2C0 08BBA2C4 ...          # a list of addresses, one type (may have "bit N")

main   = mode == 0 and level 1..99 and maxhp 1..9999
ready  = main or ta                              # when the game is really being played
show level gold elixir mode diff combo           # up to 9 plain values for the viewer's values page

when main                                        # the rules below are only looked at while this holds
57 | Adol the Shipwrecker | 5 | level >= 10      # number | title | points | rule
44 | Ys: Link to the Past I | 5 | count urns >= 10
73 | Smart Water | 10 | gold drops 999999 within 3s and elixir rises within 3s
17 | Spiritual Advantage | 10 | just gildias set and normal and spiritcape set
when ta
86 | [TA - Istersiva] | 10 | run from fighting and fight == 6 to won and moved rec6_hi unless hurt grace 0.5s
```
- **Values:** numbers are decimal or `0x..`; addresses are hex without `0x`. `float` is read as a whole number (truncated). `s32` and `u32` are the same read; comparisons are signed, so use ranges for sanity (`gold 0..9999999`).
- **Tests:** `== != < <= > >=`, `A..B` (inclusive), `set`, `off`, `and`, `or`, `not`, `( )`. `and` binds tighter than `or`.
- **`set` means "not 0 and not all ones".** Games keep "not had" as -1 (0xFFFFFFFF, 0xFF). Use `set`/`off` for flags and items, never `!= 0`. A `bit N` of a byte that is 0xFF also reads as 0. For counts that can be -1, write the range to allow it (`elixir -1..99`) or compare signed (`x >= 2` is false for -1).
- **Lists:** `count L` (how many are set), `all L`, and in runs `moved L`.
- **Over time** (looked at 60 times a second): `X drops [N]`, `X rises [N]` (by exactly N if given), `X changes`, `just <rule>` (true now, false a tick ago), `was <value>` (its value a tick ago), `<rule> within 3s` (true for that long after the rule was last true).
- **Runs:** `run from <rule> to <rule> [unless <rule>] [cancel <rule>] [grace 0.5s]`. Starts when `from` goes from false to true (and starts afresh each time it does). Unlocks when `to` goes from false to true, unless `unless` was true at any time since the start (ignoring the first `grace`). `cancel` ends it without an unlock. `moved X` (only inside `to`/`unless`) = X differs from what it was when the run started. A run cannot finish on the tick it starts.
- **Numbers** are 1..255 (use the set's own order), points 0..255, titles up to 47 characters (the pop-up shows 2 lines of 29; the viewer 42).


## How the engine treats rules
- Nothing is looked at until `ready` has held for 1 second (60 ticks). On that first look **every** rule is checked and what already holds is written down quietly and shown as one "Already earned / N found in this save" card. So: an achievement that must be seen happening (not just "is so") needs an over-time word; a plain rule will be granted from any save that already satisfies it. Felghana's Spiritual Advantage uses `just` for exactly this reason.
- Rules with **no** over-time word are looked at every 4th tick (15 times a second); rules with one, every tick. If a plain condition could be true for less than 0.07 s, add `within 0.2s` to it: that makes it an every-tick rule that also holds the result for a moment.
- A rule under `when X` is skipped while X is false, and a run's `to`/`unless`/`cancel` are skipped while the run is off. The first look after either starts has no "tick ago": `drops`, `just`, `was` answer no once. Never rely on an over-time word firing on the very tick its `when` or its run begins.
- Put the cheap identifying test in `from` (`fighting and fight == 6`), not in `to`: then only that fight's run is on.
- `and`/`or` always work out both sides (so over-time words on either side keep their history).
- A value or name used by many rules is read once per tick. Give repeated things names.
- Earned achievements are never looked at again. `earned/<ID>.txt` on the Memory Stick is the truth; the plugin re-reads it before writing and when the viewer opens (a Sidequest state restores plugin memory too).


## `ready`: write it carefully
It is the only thing between garbage memory (title screen, loading, another mode) and false unlocks.
- Include the mode value if the notes have one, and 3 or 4 values with a narrow sane range (level 1..99, max HP 1..9999, gold 0..9999999).
- Remember -1: in Felghana the count of elixirs reads -1 when none are carried, and a range of 0..99 kept the gate shut for ever.
- If the game has separate modes with their own achievements (Time Attack), make a rule per mode, `ready = main or ta`, and put each mode's achievements under its own `when`.
- No `ready` line = always ready. Don't do that.
- The viewer's values page (△) shows `show` values and "READY yes/no": if nothing unlocks, look at that page first.


## Limits (over them, lines are skipped and logged with their line number)
| What | Limit | Felghana uses |
|---|---|---|
| File size | 32 KB | 15 KB |
| Nodes (every value, test and word is one) | 1280 (`EM_MAX_NODES`) | 640 |
| List addresses, all lists together | 1024 | 356 |
| Runs | 64 | 21 |
| Names | 192 | 110 |
| Words on one line | 256 (a list of more than ~250 addresses must be split in two lists) | 85 |
| Nesting | 24 brackets/not/just/was inside one another; 40 nodes deep through names | 8 deep |
| `moved` list snapshots | 512 addresses | 130 |

A big set (150+ achievements with runs) may pass 1280 nodes. `EM_MAX_NODES` in `emblem/src/em.h` raises it (32 bytes each, static kernel memory) and needs a rebuild.


## What Felghana taught (PSP-3000, ARK-5)
- RA's US addresses worked unchanged on the undub.
- Flags in notes named after a fight starting ("Fight Dularn") may be set when the fight begins, not when it is won. Use the note's wording as a comment in the file so a misfire can be traced.
- "Show X the item" quest notes are steps, not completions; pick the step the description means and say so in a comment.
- Upgrade/level values are often stored 0-based (shown value minus 1).
- Bestiary/character tables are bytes with bit 0 = seen; use `list u8 bit 0`.
- Time-attack style modes: a state word (menu / fight / after), a "lost" byte, HP floats, and best-time records per difficulty. "Record moved" is a good finish signal (`moved rec`), with the selection byte as fallback when no record is set.
- HP values are not set up in the first half second of a fight: `grace 0.5s`.

## Buttons taken

A game file cannot add buttons, but for reference: Emblem L + R + LEFT, Scanline L + R with △ ○ □ UP DOWN, MissyHUD L + R + START, Checkpoint L + R + SELECT, Sidequest SELECT held alone, ARK's exit L + R + DOWN + START or SELECT.
