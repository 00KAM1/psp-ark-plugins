# Changelog

Each plugin has its own version number. A plugin's number goes up only when its own files change, so an entry lists only the plugins you need to replace.

## Sidequest 1.1

Replace `sidequest.prx` and `SIDEQUEST/config.txt`. Checkpoint, Scanline and Emblem are unchanged and stay at 1.0.

- **Fixed: text disappearing after a load.** After loading a state, text drawn with the PSP's own fonts could vanish (the system's save and load screens, and the text of some games such as Lunar: Silver Star Harmony) until a full power-off. A loaded state now has the PSP re-read its internal flash before anything else runs.
- **Fixed: the PSP's settings could be reset after a load.** Same cause as the missing text; the same fix covers it.
- **Battery guard.** No save or load at 5% battery or less unless the charger is connected. New setting `min_battery`.
- **Brightness after a load is now 50%** (was 70%). `load_brightness` changes it.
- **States from 1.0 cannot be loaded.** They show as empty. Save them again with 1.1.
- **Firmware 6.61 only.** The fix works with one exact build of the PSP's flash driver. On anything else Sidequest refuses to save.
- Loads take about one second longer.

## 1.0 (Initial Public Release)

All four plugins are released together as version 1.0.

### Sidequest
- **Save states for PSP games.** The whole system is frozen into a slot and brought back later, in any game started from an ISO/CSO or a PSN `EBOOT.PBP`.
- **Six slots for every game:** one suspend slot and five states, each with a picture of the screen. L / R looks at other games' slots, and loading one switches games.
- **Continue prompt.** Starting a game that has a slot offers to go straight back to the newest one.
- **Self-wake.** The PSP sleeps for each save and load and wakes itself about 3 seconds later.
- **After a load** the sound is muted and the brightness set, so a state never surprises you.
- **Safe by design:** a slot is checked in full before anything is replaced, written through a temporary file, and refused before the PSP sleeps if the Memory Stick lacks room.

### Checkpoint
- **A history of your game saves.** A dated copy is kept every time a game saves: the last 10 versions of each save, with restore and pinning.
- **Journey log.** A diary of the playthrough that writes itself from the saves' descriptions, shown as a two-page book.
- **Quit screen.** The PS button shows when you last saved, in green, amber or red, before you quit.
- **Suspend and quit** (with Sidequest): △ on the quit screen freezes the game into its suspend slot and closes it.
- **L and R on the quit screen** open Sidequest's menu and Emblem's achievement viewer.
- **Japanese text** in save descriptions and game titles is drawn in Japanese.

### Scanline
- **CRT-style scanlines over any game:** seven strengths from 12.5% to 100%, on every 2nd, 3rd or 4th row.
- **Changed in the game** with L + R and one button, held for half a second. No menu.
- **Each game keeps its own settings**, on or off included.
- **Two modes per game:** mode 1 for almost every game, mode 2 for a game whose top rows flicker.
- **A label** shows the setting and what the scanlines cost per frame.
- **No sound popping, no tearing, and never darkened twice:** frames flipped from an interrupt are drawn by the plugin's own thread, immediate flips are passed on first, and a frame shown again is recognised.

### Emblem
- **Achievements for PSP games, kept locally.** A card pops up in the bottom right corner when one is earned, and it is written to the Memory Stick. Nothing goes online.
- **One text file per game.** The plugin has no game built in; adding a game means adding a file.
- **41 games, 1,731 achievements:** 42 game files, each written or checked by hand, with AI assistance, from RetroAchievements' public memory notes.
- **Viewer** on L + R + LEFT, or R on Checkpoint's quit screen: every achievement of the game, earned or locked, with dates and points.
- **A loaded save is caught up at once:** what it has already earned is shown on one card.

### All four
- Made to run side by side, and alongside MissyHUD: no shared buttons, and the display hooks chain in any load order.
- Sliding POWER while a plugin's screen is open closes the screen first, then the PSP sleeps.
