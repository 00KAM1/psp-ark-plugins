# Sidequest + Checkpoint: how it works

Technical notes for both plugins. Sources for the firmware behaviour described here: the uOFW reverse-engineered 6.60 kernel, the ARK-5 source, and tests on a PSP-3000.

Both are kernel-mode PRX plugins loaded by ARK-5 at the `game` level. Kernel functions that have no stable import are looked up at run time with `sctrlHENFindFunction(module, library, nid)`, trying the user-library NID first, then the 6.60 driver NID, then the pre-6.60 NID.

---

## Sidequest

### The idea

When a PSP suspends, every driver saves its hardware state into RAM and the kernel saves the full CPU context. To resume, the power service calls the kernel's `sceSuspendForKernel_B2C9640B(1)`, which reloads that context. At that call, RAM holds a complete system that knows how to resume itself. Sidequest copies that system out (a save) or puts a different one in its place (a load).

### The hook

`scePower_Service`'s import of `sceSuspendForKernel_B2C9640B` is replaced by Sidequest's entry point.

- **Idle:** jumps straight to the original. Nothing is written.
- **Armed:** switches to a private 8KB stack (the power thread's own stack is 2KB), then captures or restores.

### What a snapshot contains (64MB models)

| Where | What |
|---|---|
| `0x08000000–0x09FFFFFF` | Lower 32MB: kernel, the game, and the copy of video memory the graphics driver makes at suspend |
| Media Engine save area, normally `0x0BC00000–0x0BFFFFFF` | The Media Engine's 4MB, written at suspend and read back at resume |
| Busy areas of extra RAM, 4MB or less | On the test PSP, 1MB at `0x0BB00000` |
| `0xBFC00000`, 4KB | Media Engine boot code and mailbox |

The 16KB scratchpad is not stored: the power service restores its own copy on every resume.

### Staging

The snapshot is packed into free extra RAM ("staging") while the system is suspended, then written to the Memory Stick after the system is running again.

Extra RAM is scanned partition by partition. A range that can be allocated in full is free and is held while Sidequest works. A range that cannot is in use: if it is 4MB or less it becomes part of the snapshot, otherwise Sidequest refuses to work. Staging is the largest gap between the busy areas, 27MB on the test PSP. The slot header records the areas, and a slot only loads when they match the running system's.

### Packing

Word-based run-length records: `SEEK address`, `FILL count value`, `LIT count words…`, `END`. It is simple enough to unpack from a stub with no stack. Typical slots are 5MB to 16MB; the ceiling is the size of staging.

### Capture (save)

1. The menu reserves staging, checks free space on the Memory Stick, opens a marker file, and releases the game's threads (their power callbacks must run or the suspend never completes).
2. The hook is armed, the wake alarm is set, and `scePowerRequestSuspend()` is called. The PSP sleeps.
3. On waking, inside the hook: the 4KB SRAM is copied, a context is saved, and lower RAM plus the other areas are packed into staging. If it does not fit, nothing else happens and the save is reported as refused.
4. The kernel resumes normally. The menu thread, raised to a high priority, freezes the game again before any game thread runs.
5. `[4KB header][packed stream]` is written to a temporary file, the previous slot is kept as a backup until the new one has its final name, and the picture is written alongside.

### Restore (load)

1. Checks before anything is touched: header checksum; same firmware, ARK version, model and memory layout; entry addresses inside lower RAM; the game's ISO still exists; stream checksum; and a strict walk of the stream proving it writes exactly the snapshot areas and nothing else.
2. The stream is read into staging. The game is released, the hook armed, the alarm set, a suspend requested.
3. On waking, inside the hook: the staged stream's checksum is checked again. If it changed, nothing happens. Otherwise a small unpack stub is copied to the top page of staging and run from there.
4. The stub (no stack, no globals, relative branches only) unpacks over lower RAM and the other areas, sets a flag inside the new image, flushes the data cache and invalidates the instruction cache, and jumps to the snapshot's saved context.
5. Execution continues inside the snapshot's own hook, as if it had just finished its capture. It restores its SRAM copy and calls its own kernel's resume function. The saved system resumes as it would after any sleep.
6. Its menu thread wakes first, freezes the game, refreshes the Memory Stick, cancels the alarm, applies mute and brightness, then lets the game go.

Because execution continues inside the snapshot, **a loaded slot runs the copy of Sidequest it was saved with** until the game is restarted.

### Why real sleep

The firmware has an instant suspend ("touch and go") that reaches the same resume call without powering down. On the PSP-3000 with ARK-5 it broke quitting games and loading slots, so Sidequest uses the real sleep, where the hardware comes back freshly reset and every driver rebuilds it from RAM.

### Self-wake

Sony's power code notes that a sleeping PSP resumes either from the POWER switch or after a set time through `sceRtcSetAlarmTick`. The alarm is kept by the power controller, which stays powered during sleep.

- Before each suspend: `sceRtcGetCurrentTick`, add `wake_seconds`, `sceRtcSetAlarmTick(&tick, 0)`.
- After each wake: `sceRtcSetAlarmTick(NULL, 0)`. The clock service otherwise keeps a spent alarm and re-applies it at every later resume, which made ordinary sleeps wake themselves. Passing no tick is how Sony's own module cancels it when a game exits.
- Measured on the PSP-3000: waking takes about 3 seconds from the alarm; an alarm that fired 1.5 seconds after the suspend request still woke the console.

The delay only has to cover the PSP falling asleep. Packing and writing happen after the wake.

### Memory Stick safety

A restored system remembers the Memory Stick as it was when the slot was made. Writing with that stale picture could damage files.

- A real resume re-mounts the stick, which invalidates old file handles. ARK's ISO driver reopens the game's ISO on error.
- Sidequest opens a marker file before every capture. After a restore, if that handle is dead, a re-mount happened. If it is still alive, Sidequest sends the `fatms0:` device call used after USB mode ("the stick changed behind your back") and checks again.
- The game stays frozen until this is done. If freshness cannot be confirmed, a warning is shown and must be acknowledged.

### Per-game folders

The folder name is built from the path the game was started from: the ISO's file name without extension (or the folder of an `EBOOT.PBP`), upper-cased, other characters turned to `_`, cut to 20 characters, plus `_` and four hex digits of a checksum of the whole path. This avoids relying on the disc ID, which was not always available.

Slot 0 is `SUSPEND.SQS`; slots 1 to 5 are `STATE1.SQS` to `STATE5.SQS`. A `.THM` file holds a 240x136 RGB565 picture made by averaging 2x2 pixels of the screen when the menu opens.

### Menu and game freezing

The menu thread suspends every user thread while it is open and resumes them on close. It draws directly into the frame buffer. While it is open, the system's PS-button screen is switched off through `sceImposeSetHomePopup` and the previous setting is put back on close.

### The POWER switch while a screen is open

The firmware cannot suspend around a frozen game: on the PSP-3000 the power light blinks and the console does not wake. So while a screen has the game frozen, the plugin holds a power lock (`scePowerLock`, kernel form). The power service then holds a slide of the switch as a pending request instead of starting the suspend. The input loop polls `scePowerIsRequest`; when a request appears it returns "close" to every screen, the game is resumed, the lock is released, and the pending suspend (or power-off) runs normally. The plugin waits for the request to clear before listening for buttons again.

Sidequest releases the lock before its own save and load sleeps, which would otherwise wait for it, and drops a slide that arrives in that short window (`scePowerCancelRequest`) so one sleep happens, not two. It takes the lock again once awake. Neither plugin opens its menu while the other has the game frozen. Checkpoint works the same way for its menu, journey log and quit screen, and releases the lock before handing over to Sidequest.

### Where it loads

`module_start` leaves the plugin unloaded unless the running program is an ISO game, an `EBOOT.PBP` or a disc, and is not ARK's launcher (`sctrlArkIsLauncher`, plus a path check). Saving is refused for real UMD discs.

### Tests without a PSP

- `tests/test_sim.py` loads the real `sidequest.elf` into a MIPS emulator with a PSP memory map and drives the hook as the power service would: idle pass-through, refused oversize capture, capture, refused restore after corruption, and a full A → B → A restore with every byte and register compared.
- `tests/test_host.c` covers packing, stream validation (200,000 mutated streams), slot files, formatting and config, built with AddressSanitizer.

---

## Checkpoint

### Noticing a save

Two user-mode system calls are patched with `sctrlHENPatchSyscall`: `sceUtilitySavedataInitStart` and `sceUtilitySavedataShutdownStart`. At start, the request's game name, save name and mode are recorded. At shutdown, if the result was success and the mode is one that writes, the folder `gameName + saveName` is queued.

Each folder a game writes has its own queue entry. The plugin's own thread copies a folder 3 seconds after its last write, while the game runs. A new save request waits while a copy is in progress.

### The vault

`ms0:/CHECKPOINT/<save>/<YYYYMMDD_HHMMSS>/` holds plain copies. A copy is built in `NEW.TMP` and renamed when complete; pruning happens only after the new version is in place.

- A copy identical to the newest version is skipped.
- If the newest version is younger than `min_minutes`, the new copy replaces it.
- At most `keep` unpinned versions remain.
- 4MB is left free.
- A pin is a marker file `<stamp>.PIN`. Pinned versions are skipped by pruning and by the replacement rule, do not count towards `keep`, and are limited to 10 per save.

### Journey log

After each copy, the live save's `PARAM.SFO` is read for `SAVEDATA_TITLE` and `SAVEDATA_DETAIL`. Both are reduced to a comparison key with digits (also full-width) and time and date punctuation removed. `JOURNEY/<game>.KEY` remembers the two keys last seen for each save folder. If the title key has changed, the title is logged; otherwise, if the detail key has changed, the detail is logged; `journey_detail = 1` ignores the title. The first save of a folder logs the title. Only the main save of a burst is logged: a folder written within ten seconds of another folder's save (the usual "system data" follow-up) is skipped, the same rule the quit screen uses to name the last save. The text is taken as the game wrote it (UTF-8): control characters and the field separator are removed, characters are never cut in half, and bytes that are not valid UTF-8 become `?`. It is compared with the last line logged for that same save folder; if it differs, a line `date | time played | description | save` is appended to `JOURNEY/<game>.TXT`, where `<game>` is the name the game saves under.

Time played is counted in the plugin's 20 ms loop from the system timer. A step longer than two seconds is discarded, which drops time spent in the menu, asleep, or skipped by a Sidequest load. The unsaved count is added to `JOURNEY/<game>.TIM` at each save and at quit; the file is written as `.NEW` and renamed, and a leftover `.NEW` is used if the rename was cut short.

The book is drawn with rectangles and the 8x8 font: two 216x228 pages of 18 ruled lines and 24 characters. An entry takes a date line, one or two wrapped text lines, a time line and a gap; pages are filled in order and the book opens on the last spread. Up to 120 entries are held in memory.

### Japanese text

All text from save files is handled as UTF-8 (`jtext.c`). English letters are drawn with the 8x8 MSX font; other characters are looked up by binary search in a table of 7,105 8x8 glyphs (`jfont_data.c`, 71KB, generated by `tools/make_jfont.py` from Fusion Pixel Font 8px, whose Japanese glyphs are the Misaki font). The set is JIS X 0208 plus the Windows additions. A small second table maps half-width kana to full width and accented Latin letters and curly punctuation to plain ASCII. Every character occupies one 8-pixel cell, so layout code counts characters, never bytes, and text is only ever cut between whole characters. A character with no glyph is drawn as `?`.

### Restore

Refused while a save call is active. A safety copy of the current save is forced first. Every file is then written as `<name>.NEW`; only if all succeed are extra live files removed and the new ones swapped in. A failure leaves the live save untouched.

### Quit screen

After a short start-up delay the plugin looks up `sceImposeGetHomePopup` / `sceImposeSetHomePopup` and `sceKernelInvokeExitCallback`. The feature turns on only if all are found.

The system's popup is kept switched off. The plugin's thread reads the PS button directly (a kernel-only button bit). On a press it freezes the game, makes any pending copies, and draws its screen. It ignores the press when every user thread is already suspended, which means another plugin (Sidequest's menu) has the game frozen.

X releases the game and calls `sceKernelInvokeExitCallback`, which is what the system's own screen does on "Yes". If the game is still running five seconds later, `sctrlKernelExitVSH` is used. X is disabled while a save call is active.

### Hand-over to Sidequest and Emblem

The plugins talk through kernel event flags and share nothing else. Each one creates a flag and polls it; Checkpoint finds the flags by name when the quit screen opens.

| Flag | Bit | Set by the quit screen's | What the owner does |
|---|---|---|---|
| `SidequestReq` | 0 | △ | Saves the game into slot 0 and quits it |
| `SidequestReq` | 1 | L | Opens its menu |
| `EmblemReq` | 0 | R | Opens its viewer |

Before setting a flag, Checkpoint lets the game's threads go and releases its power lock; the other plugin freezes the game again and takes its own lock. Sidequest waits 0.4 s and Emblem 0.3 s first, so the game paints over the quit screen (Sidequest takes the slot picture from the screen when its menu opens). A button is hidden when its flag does not exist. Emblem creates its flag only in a game it has an achievement file for.

### Tests without a PSP

`tests/run_tests.sh` compiles the vault and journey code for a PC with AddressSanitizer against real folders. 43 checks cover copy, the identical-copy and replacement rules, the cap, ordering, failed copies, low space, leftover temporary folders, restore and failed restore, listing, parsing of damaged save descriptions, refused path-like names, and pinning. A further 75 checks cover the journey log and text handling: when a line is added, text cleaning, Japanese and invalid text, glyph lookup, character-aware fitting and wrapping, damaged and over-long lines, failed writes, the play-time file, word wrapping, weekdays and page filling.
