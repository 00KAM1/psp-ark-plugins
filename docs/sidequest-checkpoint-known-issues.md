# Sidequest + Checkpoint: known issues and test status (v1.0)

Everything here refers to the one console these plugins have run on: a **PSP-3000 with ARK-5**, games as ISO files on the Memory Stick, launched from ARK's custom launcher. Builds in this release: Sidequest `built Nov 22 2026 02:55:43`, Checkpoint `built Nov 22 2026 02:55:44`.

**About these builds.** The results under "Confirmed on the PSP-3000" were obtained with the builds made just before this release. The release builds are the same code with the version number set to 1.0 and one addition: the quit screen's L and R buttons, which open Sidequest's menu and Emblem's viewer. That addition and the release builds themselves have not run on a PSP yet.

## Disclosure

These plugins were written by Claude, Anthropic’s AI model, from my ideas and my testing on a PSP-3000. I’m not a kernel programmer, and nothing has been tested beyond my own console. The code has had no independent review. These are kernel-mode plugins: a fault can freeze the PSP or, in the worst case, damage files on the Memory Stick. Keep a backup of your saves. Please read the known issues below before relying on them.

## Known issues

### Sidequest

**1. Text disappears from the PSP's own save/load screens after some loads.**
After loading a slot, the text in the system's save and load dialogs can vanish (title, date, details, button labels). Pictures and buttons still work, and saving still works. It stays that way across games until a full power-off: hold POWER until the light goes out, then switch the PSP on. Nothing is damaged.
- In testing the text survived when a slot was loaded the first time the Sidequest menu was opened after starting a game (which includes loads from the continue prompt), and was lost when the menu had been opened and closed before the load.
- The cause is not known. It is not corruption of the PSP's settings, and an ordinary sleep and wake does not cause it or cure it.

**2. Games that fill nearly all of their memory are refused.**
The snapshot is packed into about 27MB of spare memory before being written. A game that does not pack down that far cannot be saved: "Slot is too big for this PSP". Nothing is changed when this happens.

**3. A slot brings back the copy of Sidequest it was saved with.**
A slot holds the whole system, plugins included. After a load, the Sidequest that is running is the one from the moment of the save, until the game is restarted. After installing a new build, save your states again.

**4. Every save and load blanks the screen for several seconds.**
The PSP really goes to sleep and wakes again; this is how the snapshot is taken. An instant method exists in the firmware but on this console it broke quitting games and loading slots, so it is not used.

**5. The self-wake can miss.**
If a game is slow to go to sleep, the alarm could in principle pass before the PSP is asleep and the screen would stay dark. This has not happened in testing. Slide POWER up and the save or load finishes. Raise `wake_seconds` to 4 or 5 if it happens with a particular game.

**6. POWER while a menu is open.** A PSP cannot go to sleep around a frozen game: the power light blinks and it does not wake. So a slide of POWER while a Sidequest or Checkpoint screen is open closes the screen first, and the PSP then sleeps normally. If you ever see the blinking light, hold POWER up for ten seconds to switch off.

**6a. The game sees the SELECT press that opens the menu.**
Change `hotkey` if a game reacts badly to a held SELECT.

**7. Slots are tied to one console and one file path.**
A slot loads only on the PSP, firmware and ARK version it was made on, and only while the game's ISO or EBOOT is at the same path. Moving or renaming the ISO orphans its slots.

**8. Not for online play.**
A state taken during ad hoc or online play will not reconnect when loaded.

**9. Other plugins lose their state on a load.**
Because a load replaces the whole system, any other plugin goes back to how it was when the slot was saved. Sidequest covers the common case itself by muting and setting brightness after each load.

**10. Seen once, not reproduced:** in an early test build the PSP shut down when the PS button was pressed after a save. It never recurred.

**11. Theoretical, never seen:** the menu freezes the game's threads. If that happened at the exact moment the game was reading the Memory Stick, the plugin's own file access could wait on it.

### Checkpoint

**1. Only saves made through the PSP's standard save system are seen.** Nearly every game uses it. A game that writes its save files directly would not be noticed.

**2. Restore changes the files, not the running game.** After a restore you must load the save from the game's own load screen. Until then the game still holds its old progress and will overwrite the restored save if it saves.

**3. The quit screen takes over the PS button.** A game or plugin that also switches the system's PS-button screen on and off could conflict with it. None has been found. `quit_screen = 0` hands the button back.

**4. Quit is held back while the game is saving**, and so is restore. Wait for the save to finish.

**5. The journey log depends on what the game writes.** It follows the save's title, or its detail text when the title never changes, and ignores numbers so levels and play times do not add lines. A game whose texts never change in words gets one entry. A game that changes them in ways unrelated to progress (a party member's name, say) gets extra lines. The first entry of a game with a fixed title is that title ("Save data"); later entries are the detail. A "system data" save written just after the real save is left out, but one the game writes on its own (at start-up, say) gets a single line. The time played is Checkpoint's own count and will not match the game's clock.

**6. Japanese text is small.** Checkpoint draws Japanese with an 8x8 pixel font. Kana are clear; dense kanji are cramped. Characters outside the Japanese standard set (Korean, emoji, rare kanji) show as `?`. The journey log's text file always holds the exact text. Sidequest shows the game's file name as its title, so it is not affected unless the file name itself is Japanese, which it cannot draw.

**7. Space.** History can use up to ten times the size of each save, plus pins. Copies are skipped when less than 4MB would be left free.

### Both

- PSP-3000 only so far. The PSP-2000 is untested; the 1000, Go and Street are not supported.
- PS1 games, the XMB and ARK's launcher are not supported: the plugins remove themselves there.
- ARK-5's **Extra RAM** must be `Off` for Sidequest.

## Test status

**Confirmed on the PSP-3000**

| Area | What was checked |
|---|---|
| Sidequest save and load | Save, load in the same game, load after playing on, switching between games' slots |
| Sidequest pictures | Slot pictures in the menu |
| Sidequest continue prompt | Prompt appears at game start; continuing from it loads the slot |
| Sidequest self-wake | Save, suspend-and-quit and load all wake by themselves at 3 seconds |
| Sidequest alarm cancel | An ordinary POWER sleep stays asleep after a save or load |
| Sidequest L / R | Browsing other games' slots |
| Sidequest after-load settings | Mute and 70% brightness applied after a load |
| Sidequest in the launcher | Plugin stays out of ARK's custom launcher |
| Checkpoint save detection | Copies made when a game saves, including a game that writes two folders; game ID shown |
| Checkpoint menu | Lists saves and versions |
| Checkpoint pinning | Pin and unpin a version |
| Checkpoint journey log | Book opens from the menu, shows the title page and an entry with date and time played; closing it leaves the menu clean |
| Japanese text | A Japanese game title and save descriptions are drawn in Japanese in the menu and the journey log |
| Checkpoint quit screen | PS button opens it; O returns to the game; X quits to the launcher |
| Suspend and quit | Hand-over from Checkpoint to Sidequest, slot written, game closed |
| POWER slide on a menu | The menu closes and the PSP sleeps instead of hanging |

**Not yet confirmed on a PSP**

| Area | Status |
|---|---|
| Quit screen: L and R | New in this release. Opening Sidequest's menu and Emblem's viewer from the quit screen has not run on a PSP |
| Journey log entries | The rule that follows the title or the detail and ignores numbers has not run on a PSP. A second entry after moving to a new place, and system data being left out, have not been reported |
| Checkpoint restore | Logic passes its PC tests; not reported from hardware |
| Sidequest state slots 1 to 5 in detail | Overwrite, delete, and all five in use have not been walked through one by one |
| PS-button block while the Sidequest menu is open | Built; not explicitly confirmed |

**Tested on a PC only**

- Sidequest: packing and unpacking, slot files, a full capture and restore in a MIPS simulator with every byte and register compared, 200,000 damaged-slot inputs refused.
- Checkpoint: copying, history limits, the 10-minute rule, restore and failed restore, pinning, damaged save descriptions (43 checks); the journey log's file handling, wrapping, paging, Japanese text and the choice of entry text (75 checks).

## Reporting a problem

Include the game, what you pressed, and the log file (`ms0:/SIDEQUEST/sidequest.log` or `ms0:/CHECKPOINT/checkpoint.log`). The log's `built` line identifies the exact build.
