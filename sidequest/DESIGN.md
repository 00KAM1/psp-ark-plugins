# Sidequest: design notes

## The idea

When a PSP suspends, every driver saves its state into RAM and the kernel saves the full CPU context (general, FPU, VFPU and COP0 registers) into `g_suspendCtx`. To resume, `scePower` calls the kernel's `sceSuspendForKernel_B2C9640B(1)`, which reloads that context. So at that call, RAM holds a complete system that knows how to resume itself. Sidequest saves that system and can later put a different one in its place.

Sources for everything below: uOFW's reverse-engineered 6.60 kernel (`power/powerSwitch.c`, `sysmem/start.S`, `sysmem/sysmem.c`, `ge/ge.c`, `me_wrapper/me_wrapper.c`) and the ARK-5 source.

## What a suspended system consists of (64MB models)

| Where | What | In the slot? |
|---|---|---|
| `0x08000000–0x09FFFFFF` | kernel, VRAM save (the GE copies eDRAM into the vshell partition), game | yes |
| ME save area, normally `0x0BC00000–0x0BFFFFFF` | the Media Engine's 4MB, written by the ME on suspend and read back on resume | yes |
| `0xBFC00000`, 4KB SRAM | ME boot code and mailbox | yes (kept in the plugin's memory) |
| scratchpad `0x00010000` | 16KB | no need: scePower restores its own copy on every resume |
| syscon scratchpad 0xC | pointer to the resume data, used only when waking from real sleep | not needed, see below |

The ME save address comes from `sceKernelGetMEeDramSaveAddr()`, with partition 11 as a fallback. If neither gives a sane answer, Sidequest refuses to work.

## The hook

`scePower_Service`'s import of `sceSuspendForKernel_B2C9640B` is replaced by `sq_hook_entry`. That call happens at the end of an instant "touch and go" suspend (`scePowerRequestSuspendTouchAndGo`, no power-off) and also when waking from real sleep, so both modes use the same code.

- **Idle:** jump straight to the original. Nothing is written.
- **Armed:** switch to a private 8KB stack (scePower's thread stack is only 2KB), then capture or restore.

## Capture

1. The menu reserves staging, thaws the game (its power callbacks must run or the suspend never completes), arms the hook and requests a suspend.
2. In the hook: copy the SRAM, save a setjmp-style context, then pack lower RAM and the ME area into staging. Records are `SEEK addr`, `FILL n value`, `LIT n words...`, `END`.
3. If it doesn't fit, nothing else happens and the save is reported as refused.
4. The kernel resumes. A wake event raises the menu thread (priority 6) so it freezes the game again before any game thread runs, then writes `[4KB header][packed stream]` to `SLOTn.TMP` and swaps it in.

**Staging and busy areas.** Extra RAM is scanned partition by partition. A partition range that can be allocated in full is free and is held while we work. One that can't is in use: if it is 4MB or less it becomes part of the snapshot (on a real PSP-3000 with ARK-5 this is partition 8, `0x0BB00000–0x0BBFFFFF`, found in the first hardware test), otherwise we refuse. Staging is the largest gap between the busy areas (27MB on that PSP). So nothing of the running system lives outside the snapshot, and nothing in use is written to. The slot header records the areas, and a slot only loads when they match the running system's.

## Restore

1. Checks before anything is touched: header CRC, same firmware / ARK / model / ME area, entry addresses inside lower RAM, the game's ISO still exists, stream CRC, and a strict walk of the stream proving it writes exactly lower RAM and the ME area and nothing else.
2. The game is thawed, the hook armed, a suspend requested.
3. In the hook: the staged stream's CRC is checked again. If it changed, nothing happens. Otherwise the unpack stub is copied to the top page of staging and run from there.
4. The stub (no stack, no globals, relative branches) unpacks over lower RAM and the ME area, sets a flag inside the new image, flushes the data cache and invalidates the instruction cache with the same instructions the kernel uses, and jumps to the snapshot's saved context.
5. Execution continues inside the snapshot's own hook. It restores its SRAM copy (except 0x200–0x21F, which belong to the wake-up code) and calls its own kernel's resume function. The saved system resumes as it would after any suspend.
6. Its menu thread wakes first, freezes the game, and refreshes the Memory Stick before anything may write to it (below). Then the game continues.

Because the stub flushes both caches and nothing of the replaced system is used afterwards, the two systems do not need the same memory layout. The simulation restores with the plugin loaded at a different address to check this.

## Memory Stick safety

A restored system remembers the stick as it was when the slot was made. Writing with that stale picture could damage files. Defences:

- A real resume re-mounts the stick, which is why file handles die across sleep and why ARK's Inferno driver reopens the ISO on error (`Inferno/src/iodrv_funcs.c`).
- Sidequest opens a file before every capture. After a restore, if that handle is dead, a re-mount happened. If it is still alive, Sidequest sends `fatms0:` devctl `0x0240D81E`, the "stick changed behind your back" call also used after USB mode, and checks again.
- The game stays frozen until this is done. If freshness can't be confirmed, a warning is shown and the user must acknowledge it.
- Slots are written to a temporary file and swapped in, with the old slot kept until the new one has its final name.

## Verified in simulation (`tests/`)

`test_sim.py` loads the real `sidequest.elf` (relocated) into a MIPS emulator with a PSP memory map and drives the hook exactly as scePower would: idle pass-through, refused oversize capture, capture, refused restore after corruption, and a full A → B → A restore with every byte and register compared. `test_host.c` covers packing, stream validation (200,000 mutated streams), slot files, formatting and config, built with AddressSanitizer.

## What only a real PSP can answer

| # | Question | Evidence so far | How we find out |
|---|---|---|---|
| H1 | Is scePower's import hooked on 6.61, and does touch-and-go exist and work on retail units? | NIDs from uOFW 6.60 | `probe.log`: `hooked=1`, `instant=` non-zero; step 2 |
| H2 | Is the staging window really free with ARK-5's Extra RAM and Inferno Cache off? | partition table in sysmem | `probe.log` partition lines; save refuses otherwise |
| H3 | Can interrupts stay off for the 1–3 s that packing takes? | the kernel's own suspend does long copies here | step 2 |
| H4 | Does a game's memory pack below 28MB? | unknown per game | the menu's "needs N of 27 MB" |
| H5 | Do all drivers resume correctly when hardware was left in another system's suspended state? | resume handlers reprogram hardware from RAM | steps 3 and 4. `deep_sleep = 1` restores at a real wake-up, where hardware is freshly reset |
| H6 | Does the stick refresh behave as described, and does the game's ISO reopen? | Inferno source, USB-mode practice | `sidequest.log` after step 3; check the stick on a PC |
| H7 | Are the cache instructions right on a real Allegrex? | copied from the kernel's routines | step 3 |

## Not done yet

- Loading from the XMB, without starting a game first.
- Real compression (LZ), for games that use nearly all their memory.
- PSP Go support through its built-in hibernation file.
