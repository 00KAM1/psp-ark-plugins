# Runtime 1.0

**Runtime is a companion to the [Shelf launcher](https://github.com/00KAM1/psp-shelf-launcher) and does nothing you can see without it.** It has no screen, buttons or menu. It keeps a record on the Memory Stick of how long the PSP has been on and what that cost in battery, and Shelf's PS-button menu shows that record. Install it only if you use Shelf.

> **Test status.** Runtime has run on a PSP-3000 (firmware 6.61, ARK-5) with Shelf and works there. The build in this release, `built Nov 24 2026 08:13:05`, is the one that ran. The four points under [To check first on a PSP](#to-check-first-on-a-psp) have not been reported one by one.

Runtime counts in every program: the XMB, PSP games, PS1 games, homebrew and launchers. It draws nothing and hooks nothing. Without it, Shelf keeps a simpler count of its own, in which time asleep counts as "on". Without Shelf, Runtime's logs are still written, but nothing displays them.

## Install

Copy `runtime.prx` to `ms0:/SEPLUGINS/` and add this line to `PLUGINS.TXT`, then restart:

```
always, ms0:/SEPLUGINS/runtime.prx, on
```

## What it writes

Every two minutes Runtime reads the clock and the battery and brings three files in `ms0:/RUNTIME/` up to date. The folder is Runtime's own: Shelf reads it and never writes to it, and Runtime never writes to Shelf's folder.

`uptime.txt`, the power-on in progress (Shelf reads this).

`battery.log`, one line per finished power-on, sleep counted apart:

```
Tue Nov 24 09:00 to Tue Nov 24 13:07 | 1h 07m on, 3h 00m asleep | battery 100% to 88% | -12%
```

`battery_use.log`, the battery at power-on, at every program start, on waking, and every 5 percent. It is set aside as `battery_use.old` at 128 KB.

```
Tue Nov 24 09:00 | 100%          | power on XMB
Tue Nov 24 09:03 |  99%          | start Ys.iso
Tue Nov 24 09:23 |  94%
Tue Nov 24 14:00 |  88%          | woke
```

`runtime.log` holds one line from the last time the plugin was loaded: build, program, whether the battery reading was found.

## How it tells things apart

- No reading for more than 5 minutes, and the plugin has only just been loaded: the PSP was off. A new power-on begins and the old one gets its line in `battery.log`.
- No reading for more than 5 minutes, and the plugin was in memory all along: the PSP was asleep. The gap is added to the time asleep.
- Off and on again within 5 minutes counts as the same power-on.
- The clock set back does not start a new power-on.

## How it works

Kernel plugin, loaded at ARK's `always` level. One thread. In a game or a launcher it takes its first reading in `module_start`, so the record is fresh before the program runs; in the XMB it waits 10 seconds, then retries until the Memory Stick can be written.

The record is `start start% seen seen% 2 asleep mark%`: times in minutes since 2000 (local time), battery percentages (-1 for none), the owner flag 2, minutes asleep, and the battery level at the last line of the use log. Shelf's own count uses the first five fields with flag 0 or 1; Runtime takes such a record over.

**Sidequest loads.** Loading a save state puts every plugin's memory back to the moment of the save. Runtime therefore keeps nothing between two readings: each time it reads the file, changes it and writes it back. The one thing it does keep in memory is "this is my first reading since I was loaded". That is safe because a freshly loaded copy always writes the file before a state can be loaded, so a restored copy always finds a fresh file.

The battery functions are looked up with `sctrlHENFindFunction` (`scePower_Service` / `scePower`, 0x2085D15D for the percentage, 0x1E490401 for charging) and never linked. Every linked import is one the other plugins in this repository already use.

The logic lives in `src/track.c`, which has no PSP calls; 24 PC checks cover it (power-on, restart, state load, sleep, clock set back, taking over Shelf's record, damaged files, the use log).

## To check first on a PSP

1. `ms0:/RUNTIME/runtime.log` exists after a restart and says "battery reading found".
2. The times in `battery_use.log` match the PSP's clock, especially the line written in the XMB right after power-on (is the time zone ready that early?).
3. No hitch in a game every two minutes (one small read and write).
4. After a Sidequest load, "On for" in Shelf has not jumped.

## Known limits

- It writes into `ms0:/RUNTIME/` also when Shelf is not installed.
- If `uptime.txt` there cannot be read as a record, Shelf shows no time or battery lines until it can.
- A power cycle shorter than 5 minutes is not seen.

## Disclosure

This plugin was written by Claude, Anthropic’s AI model, from my ideas and my testing on a PSP-3000. I’m not a kernel programmer, and nothing has been tested beyond my own console. It runs in kernel mode in every program.
