# Emblem: how it works

Technical notes. Sources for the firmware behaviour described here: the PSPSDK headers and tests on a PSP-3000.

Emblem is a kernel-mode PRX plugin loaded by ARK-5 at the `game` level. It has three parts: a rule engine with no PSP code in it, a display hook that draws the pop-up card, and a viewer that draws over the frozen game. It reads the game's memory and never writes to it.

## Start-up

- The plugin stays loaded only in PSP games (an ISO, an `EBOOT.PBP` or a disc), not in the XMB, PS1 games or ARK's launcher.
- Four seconds after the game starts it reads the game's ID (`DISC_ID`, from the system or from the disc's `PARAM.SFO`) and looks for `ms0:/EMBLEM/games/<ID>.txt`.
- No file: the display hook is taken out again, its thread ends, and the plugin does nothing more.
- A file: it is read into a fixed 32KB buffer and compiled into a table of nodes. Lines that cannot be read are logged with their line number and skipped; the rest of the file still counts.

All memory is static (about 126KB). Nothing is allocated.

## The rule engine

`src/em.c` has no PSP calls and no C library calls, so the same file is built into the plugin and into the PC tests.

- Every value, test and word of a game file becomes a node. A node used by many rules is worked out once per look and remembered until the next.
- The control thread looks at the game every 16 ms (a tick). Memory is read only inside the game's user RAM (`0x08800000` to `0x09FFFFFF`); an address outside it, or an unaligned 16- or 32-bit read, gives 0.
- **`set` means "not 0 and not all ones".** Games keep "not had" as -1 as well as 0. A bit of a byte that is `0xFF` reads as 0, and a float of `0xFFFFFFFF` reads as 0.
- **Nothing counts until `ready` has held for one second.** The file's `ready` rule is the only thing between leftover memory on a title screen and a false unlock.
- **The first look after that takes every rule at once.** What a loaded save already holds is written down quietly and shown on one card.
- **After that, rules are split by kind.** A rule with an over-time word (`drops`, `rises`, `changes`, `just`, `was`, `within`, a run) is looked at on every tick. A rule that only asks how things are now is looked at every 4th tick.
- **A rule under a `when` that does not hold is skipped**, and the inside of a run is skipped while the run is off. The first look after either starts has no "tick ago", so over-time words answer no once.
- `and` and `or` always work out both sides, so over-time words on either side keep their history.
- Earned achievements are never looked at again.

Measured on the compiled plugin in a MIPS emulator with the Felghana file (89 rules): about 11,500 instructions per look on average, 36,000 on the every-4th-tick look.

## The earned file is the truth

A Sidequest state restores the whole system, this plugin's memory included. So the plugin's own list of what is earned cannot be trusted after a load. `earned/<ID>.txt` is read again before any unlock is written and when the viewer opens. A number listed twice keeps its first date. The file is read in pieces and can be any length.

## The display hook

The hook is Scanline's: the first two instructions of `sceDisplaySetFrameBuf` are replaced with a jump and run from a trampoline, so it chains with Scanline's own hook and with MissyHUD's system-call hook in any load order. See [Scanline: how it works](scanline-how-it-works.md#the-hook).

The hook does nothing unless a card is showing. When one is:

- **A call from a thread for the next refresh:** the card is drawn, then the call is passed on.
- **An immediate call:** passed on first, then the card is drawn.
- **A call from an interrupt handler** (Ys: The Oath in Felghana does this on every frame): nothing is drawn there. An event flag wakes the plugin's own thread, which draws as soon as the interrupt is over. The card sits at the bottom of the screen, which the display reaches about 13 ms later.

The card is drawn through the uncached address of the frame, 288 pixels wide, 8 pixels from the bottom right corner. It slides in for 0.25 s, stays 5 s and slides out. Up to eight wait in a queue.

## The viewer

The viewer handles the game, the PS button and the POWER switch the way Checkpoint's menu does:

- The game's user threads are suspended while it is open, and the hook draws nothing.
- The system's "quit the game?" screen is kept away.
- A power lock is held, so a slide of POWER waits. The viewer notices, closes itself, lets the game go, and the PSP then sleeps.
- It does not open while every user thread is already suspended, which means another plugin has the game frozen.

It opens on L + R + LEFT held for half a second, or when bit 0 of the event flag `EmblemReq` is set, which Checkpoint's quit screen does for its R button. The flag is only created once a game file has loaded.

`src/draw.c` has no PSP calls, so the tests build it on a PC.

## Tests without a PSP

- `tests/test_engine.c` and `tests/test_files.c`: 84 checks built with AddressSanitizer. They cover the file format, every word of the rule language, the limits, the earned file, the viewer's drawing in all four pixel formats, and the Felghana file on a fake game memory. `tests/reference/` holds hand-written C rules for Felghana from an early test build; with every rule looked at on every tick, the game file must agree with them tick for tick over random play.
- `tests/test_sim.py` loads the compiled `emblem.elf` into a MIPS emulator: 30 checks of the display hook and of the compiled engine on the real Felghana file.
