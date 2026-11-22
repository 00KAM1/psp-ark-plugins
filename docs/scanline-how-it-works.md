# Scanline: how it works

Technical notes. Sources for the firmware behaviour described here: the PSPSDK headers and tests on a PSP-3000.

Scanline is a kernel-mode PRX plugin loaded by ARK-5 at the `game` level. It allocates no memory. Kernel functions that no earlier plugin of mine had already imported on this console are looked up at run time with `sctrlHENFindFunction(module, library, nid)` and never linked: a kernel module that asks at load time for a function the system does not have is refused, silently.

## The hook

Every frame a game shows goes through `sceDisplaySetFrameBuf(address, stride, format, sync)`. Scanline darkens rows of that frame in place.

Most plugins take over the call with `sctrlHENPatchSyscall`. That replaces a system-call table entry only if it still holds the original address, and reports nothing when it does not, so two plugins doing it on the same call means one silently loses, depending on load order. MissyHUD hooks this call that way.

Scanline instead replaces the first two instructions of the function itself with `j hook; nop` and runs the two moved instructions from a four-word trampoline that jumps back to the third. A table hook loaded before or after it still calls the original address, which now leads through Scanline, so both work in either order. If the first two instructions cannot be moved (a branch), it falls back to the table and says so in the log.

A call is ignored unless the frame lies whole inside the 2MB of video memory or inside main RAM, with a sane stride and format.

## Darkening a row

A dark row is processed a 32-bit word at a time. In the 16-bit formats (565, 5551, 4444) a word holds two pixels. Shifting the whole word right by *k* divides every colour channel by 2^k; a mask then clears the bits that slid in from the neighbouring channel or pixel. Each strength is a sum of such terms:

| Strength | Kept | As shifts |
|---|---|---|
| 12.5% | 7/8 | `p - (p >> 3)` |
| 25% | 3/4 | `p - (p >> 2)` |
| 37.5% | 5/8 | `(p >> 1) + (p >> 3)` |
| 50% | 1/2 | `p >> 1` |
| 62.5% | 3/8 | `(p >> 2) + (p >> 3)` |
| 75% | 1/4 | `p >> 2` |
| 100% | 0 | alpha only |

The fractions add up to less than one, so a sum never carries from one channel into the next. Alpha bits are kept. The 565 format has no alpha and gets loops with that work compiled out.

An eighth is the smallest piece. With 5 bits per colour, a sixteenth would not change levels below 16 at all.

## Reading video memory

The frame is read and written through the CPU's data cache (`address | 0x80000000`), in 64-byte lines, which measured 2.7 ms a frame against 7.7 ms for uncached access one word at a time.

The graphics chip draws into video memory behind the CPU's back, so before a row is read its cache lines are dropped, and after it is written they are written back and dropped again (`sceKernelDcacheWritebackInvalidateRange`), for the dark rows only. An early build emptied the whole data cache twice a frame instead; nothing else in the system expects that from a per-frame hook, and it is not done.

## Never twice

Games often show the same buffer again without redrawing it: pauses, loading screens, a 30 fps game on a 60 Hz screen. Darkening it again each time would fade it to black.

Each dark row is cut into four segments of 120 pixels. In each, the plugin remembers the first and the last word that a second pass would still change, and what it left there:

- The lowest bit of each colour in a remembered word is overwritten with a pattern that depends on the place, not the frame. In the 16-bit formats the word's two pixels get opposite patterns, so no flat-colour word can equal it. In 8888 the low two bits are used and the neighbouring pixel gets the opposite bits.
- The stamped word is never the value the game itself had there, so a game redrawing the very same picture is seen as a redraw.
- A 16-bit sum of the two words beside each remembered word is kept as well. In dark pictures darkening changes little, and a noisy video would now and then produce two pixels equal to a stamped word by chance; it then has to hit six.

On the next flip of that buffer, a segment is left alone if either of its two marks is still there and darkened again if both are gone. Two marks, one at each end, because an overlay redrawn on every flip (a HUD) usually covers one end of a segment at most; the other end then says "still dark" and the picture beside the overlay is not darkened over and over. Segments, because a game that redraws part of the screen gets that part darkened again and the rest left alone.

Up to three buffers are remembered, for triple buffering. Any change of settings forgets them all.

## Where the call comes from

How the frame is handled depends on who calls and how.

**From a thread, for the next refresh** (`sync = 1`). The usual case. The plugin asks the graphics chip whether it has finished the frame (`sceGeDrawSync(1)`, the form that returns at once, never the waiting form). If it is still drawing, the plugin sleeps in 250 µs steps until it is done, 12 ms at most; a list the game has paused is not waited for. Then it darkens the frame and passes the call on.

**From a thread, immediately** (`sync = 0`). The game has waited for the refresh itself and wants the picture changed now. Holding the call back for the 3 ms of a pass moved the change some forty rows down the screen and tore the picture there. So the call is passed on first and the frame darkened straight after. The graphics chip is asked here too, with quick repeated checks for the first 0.6 ms before any sleeping.

**From an interrupt handler, or with interrupts off.** Some games change the picture from their vertical-blank interrupt. Nothing else runs while a handler does, sound included: a 2.7 ms pass there made the sound pop once a frame. So nothing is drawn there. The request is noted, an event flag is set, and the handler returns. A kernel thread of the plugin's (priority 15, ahead of every game thread) wakes as the interrupt ends and draws the frame. It also looks for a request by itself every millisecond.

Inside a hooked system call the `k1` register still marks the call as coming from user mode, and the kernel refuses to touch its own objects on that basis (`0x800200D1`). The plugin clears `k1` around the event-flag call and puts it back.

**Mode 2.** The three cases above are mode 1. In mode 2, chosen per game, every call is handled like the first case wherever it comes from: the frame is darkened, then the call is passed on. The frame is then complete when it is first shown. Mode 1 cannot promise that in a game that changes its picture late and takes long to draw: on the PSP, a 59 fps game at 7.1 ms a frame, and still at 4.5 ms, showed the top rows before the plugin had reached them. The price is the one mode 1 was made to avoid: the change is held back for the length of the pass, or the pass runs inside the game's interrupt.

## Staying ahead of the screen

In the last two cases of mode 1 the frame is already on its way to the screen while the lines are drawn. The pass moves down the picture several times faster than the screen is scanned out, so:

- The frame is done in bands of 40 rows (about 0.4 ms each), each one flushed, darkened and flushed again before the next is touched. A band's rows only reach the screen's memory when the band is finished, and the first band has to be there before the screen shows its first row.
- The plugin asks which row the screen is on (`sceDisplayGetCurrentHcount`). If the screen is already part-way down, the pass starts six rows below that, runs to the bottom, and then does the rows above, which are not shown again until the next refresh.
- The label is drawn before any darkening and again after its band, so it is never caught half-done.

## Per-game settings

The game is identified by the path it was started from (`sctrlSEGetUmdFile`, else `sceKernelInitFileName`): the ISO's file name without extension, or the folder of an `EBOOT.PBP`, upper-cased, other characters turned to `_`, plus `_` and four hex digits of a checksum of the whole path. This is the same scheme Sidequest uses for its folders.

`games.txt` is read once at start. It is written 3 seconds after the last button change, through a temporary file that is then renamed; other games' lines are kept as they are. It is capped at 8KB, oldest lines dropped first.

## Buttons

A control thread reads the pad every 30 ms. L + R with exactly one other button must be held for 500 ms before it counts; UP and DOWN then repeat every 400 ms. Nothing is intercepted.

## Tests without a PSP

- `tests/test_host.c` builds the pixel code and the settings code for a PC with AddressSanitizer: 53 checks, compared against a plain per-channel reference.
- `tests/test_sim.py` loads the compiled `scanline.elf` into a MIPS emulator against a stand-in display function and drives the hook as a game would: 50 checks. The emulator has no Allegrex `min` / `max` instructions, so the test carries those out itself.

Neither can show timing, real caches or a real screen. Those were settled on the PSP.
