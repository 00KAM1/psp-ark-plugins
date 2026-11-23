# Emblem: known issues and test status (v1.0)

Everything here refers to the one console this plugin was made for: a **PSP-3000 with ARK-5**, games as ISO files on the Memory Stick, with Scanline and MissyHUD installed alongside. Build in this release: `built Nov 22 2026 03:34:23`.

## Disclosure

This plugin was written by Claude, Anthropic’s AI model, from my ideas and my testing on a PSP-3000. I’m not a kernel programmer, and nothing has been tested beyond my own console. The code has had no independent review. This is a kernel-mode plugin: a fault can freeze the PSP. It reads the game's memory and never writes to it, and it writes only inside `ms0:/EMBLEM/`. Please read the known issues below before relying on it.

## Known issues

**1. The release build has not been confirmed on the PSP yet.**
Two early test builds ran on the PSP-3000 in Ys: The Oath in Felghana: the card drew correctly next to another plugin, the memory values matched the game, and an achievement unlocked on loading a save. The release has the same display code and the same Felghana rules, checked against those builds on a PC (apart from the difficulty change in issue 5), plus the game-file engine and the viewer, which have only run on a PC.

**2. Only one game file has been played on a PSP.**
All 42 files were written or checked by hand, with AI assistance, against RetroAchievements' public memory notes, and on a PC each loads without errors and unlocks nothing from empty memory. Only Ys: The Oath in Felghana has been played with Emblem. A rule in any other file can still be wrong: if an achievement pops when it should not, delete its line from the file.

**3. Most files cover part of a set.**
A file holds only what the notes show plainly. Challenge runs are mostly left out, and the first lines of each file list what is missing. Emblem's achievement numbers stop at 255, so Valkyria Chronicles 2 (314 in the set) and Zero no Kiseki (257) could never be complete.

**4. One file may never unlock anything.**
In Star Ocean: First Departure the "game is running" check rests on a value that has not been confirmed. If it is wrong, the file stays silent. The viewer's values page shows `READY`.

**5. Some rules are looser than the achievement's description.**
- Ys: The Oath in Felghana: bosses, the bestiary and the combo achievements count on any difficulty, where the set asks for Normal or higher. The Hard, Nightmare and Inferno achievements still need that difficulty, and Time Attack still needs Normal or higher.
- Valkyrie Profile: boss wins count whatever the difficulty or weapon.
- Sunday vs Magazine: the support lists count only the unlockable supports the notes name.
- Tenchu: mask fragments 11 to 20 assume their bytes follow the ten the notes name.
- Tetris: "from level 1" is not checked.
- Jeanne d'Arc: stages unlock only when they are cleared while Emblem is running; an old save does not catch up.

**6. Fan translations, undubs and other regions may not match.**
The memory notes do not say which release they are for. The Felghana file was made for the US release and works on its undub. The files for Japanese discs that are usually played with a fan translation (.hack//Link, Danganronpa, Digimon World Re:Digitize, Corpse Party: Sweet Sachiko, Ys vs. Sora no Kiseki, Zero no Kiseki, Soul Eater, Sunday vs Magazine and others) and for the European discs of Crisis Core and Dissidia 012 use the addresses as the notes give them. If a disc differs, the file most likely stays silent.

**7. In Felghana, some rules rest on a reading of a note.**
Not seen on the PSP yet: everything in Time Attack and Boss Rush, the weapon and armour upgrade levels, the treasure chest lists, and whether the first Dularn achievement pops when the fight starts instead of when it is won. Six of the 95 achievements have no memory note and are left out.

**8. Loading a save or a state grants what it already holds.**
Achievements about "having" something unlock from any save that has it, including a Sidequest state made later in the game.

**9. A plain rule can come up to 0.05 s late.** Rules that only look at how things are now are checked 15 times a second, the others 60 times.

**10. It takes a little CPU time from the game.**
In the emulator the Felghana file costs about 11,500 MIPS instructions per look on average, 60 looks a second. The time on a real PSP has not been measured. In a game with no file the plugin switches itself off.

**11. The L + R + LEFT press is seen by the game as well.**

## Test status

**Confirmed on the PSP-3000 (early test builds, Ys: The Oath in Felghana, US undub)**

| Area | What was checked |
|---|---|
| Card | Drew as in the PC render, in the bottom right corner, with another plugin's display hook loaded before it |
| Memory values | Gold, EXP, HP, level and max HP read from the notes' addresses matched the game |
| "Not had" is -1 | The game keeps -1, not 0, for things not carried. The first build never unlocked anything because of it; fixed |
| An unlock | "Getting to Know You" unlocked on loading a save and was written to the Memory Stick |

**Not yet confirmed on a PSP**

| Area | Status |
|---|---|
| The release build as a whole | No report yet |
| Opening the viewer from Checkpoint's quit screen | New in this release; not run |
| The viewer | Drawn on a PC only. Its freeze, PS-button and POWER handling is Checkpoint's, which is confirmed there |
| The "already earned" card | The one unlock seen came as a normal card |
| The 41 game files other than Felghana's | None has been played |

**Tested on a PC only**

- 84 checks built with AddressSanitizer: the file format and its error messages, every word of the rule language, runs, the limits, the earned file (long files, damaged lines, a number listed twice), the viewer staying inside its panel in all four pixel formats, and the Felghana file against a fake game memory. With the same inputs, the game file and the hand-written rules of the test build confirmed on the PSP (with the same difficulty change) unlock the same achievements on the same ticks over 120,000 ticks of random play.
- 30 checks that run the compiled plugin in a MIPS emulator: the display hook (attaching, detaching, chaining behind another plugin, frames from an interrupt handed to the plugin's thread, the card drawn exactly as the PC render), and the compiled engine loading the real Felghana file and unlocking from the memory values seen on the PSP.
- All 42 game files load with no errors and unlock nothing from empty memory.

## Reporting a problem

Include the game, what popped or did not, `ms0:/EMBLEM/emblem.log`, and a photo of the viewer's values page (L + R + LEFT, then △).
