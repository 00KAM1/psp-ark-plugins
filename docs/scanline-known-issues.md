# Scanline: known issues and test status (v1.0)

Everything here refers to the one console this plugin has run on: a **PSP-3000 with ARK-5**, games as ISO files on the Memory Stick, with MissyHUD installed alongside. Build in this release: `built Nov 22 2026 02:55:46`. It is the build confirmed on the PSP (`Nov 21 2026 12:43:24`) compiled again with the version number set to 1.0 and nothing else changed, and it has run on the PSP.

## Disclosure

This plugin was written by Claude, Anthropic’s AI model, from my ideas and my testing on a PSP-3000. I’m not a kernel programmer, and nothing has been tested beyond my own console. The code has had no independent review. This is a kernel-mode plugin: a fault can freeze the PSP. It writes only inside `ms0:/SCANLINE/`. Please read the known issues below before relying on it.

## Known issues

**1. Games that never hand frames over get no scanlines.**
The plugin works on frames passed to the system's `sceDisplaySetFrameBuf` call. A game that sets its screen up once and never calls it again is not seen. MissyHUD's notes name GTA: Liberty City Stories as one such game; I have not tried it.

**2. The picture is darker.**
At 50% on every 2nd row about a quarter of the brightness goes; at the default 25%, about an eighth.

**3. It takes CPU time from the game, and a trace of sound popping can remain.**
The lines are drawn by the CPU: about 3 ms a frame for a 16-bit game on the PSP-3000 (measured: 2.7 to 3.0 ms). A 32-bit game costs about three times as much. A game that already uses all its time could slow down or lose sound quality. The label's `COST` figure shows the real number for the game you are in; wider spacing costs less. In a game that changes its picture from inside an interrupt (Ys: The Oath in Felghana), a faint pop can still be heard now and then.

**4. In some games the top few rows, or the label's top line, flicker or lose their lines.**
Seen in one 59 fps game where the lines cost 7.1 ms a frame: the game changes its picture so late that the screen is already showing the top rows when the plugin starts on them. Wider spacing did not cure it. Mode 2 (L + R + □ in that game) draws the lines before the picture changes instead, and cured it there. It can tear the picture or pop the sound in some games, which is why it is a per-game choice and not the default.

**4a. A game that redraws only part of the screen can lose lines on that part.**
If a game keeps a picture and repaints only a region of it, that region has no lines until the game next redraws the whole frame, and a strip of up to 120 pixels at its edge can lose them as well. Nothing around it is ever darkened twice.

**5. A game that reuses its last shown frame will see the lines build up.**
Some games copy the previous frame forward and draw on top, or use it for blur effects. The lines are part of that frame, so they are copied and darkened again. Turn Scanline off for that game (L + R + △).

**6. Two words per 120 pixels of each dark row are changed by one brightness step.**
This is how the plugin recognises a frame it has already darkened. It is one step in 32, on a few pixels, in the same places every frame.

**7. Very rarely a short piece of one line can be missing for one frame.**
If a game's freshly drawn pixels happen to match the plugin's marks exactly, a 120-pixel piece is taken for already done. Test builds showed it now and then in dark video. The release compares more pixels and it has not shown since in the same videos. It cannot be ruled out entirely.

**8. The L + R buttons are seen by the game as well.**
And a game that has you hold L + R with a face button for half a second will change your scanlines. Set `hotkeys = 0` once you have settings you like.

**9. Renaming or moving a game's ISO resets its settings.** The settings are tied to the file's path.

## Test status

**Confirmed on the PSP-3000**

| Area | What was checked |
|---|---|
| Scanlines in a game | Ys: The Oath in Felghana (16-bit, 30 fps) at the default settings: lines steady across the whole screen |
| Sound | Ys changes its picture from inside an interrupt on every frame. With the lines drawn by the plugin's own thread the popping heard in test builds is all but gone: barely there, and rarely |
| No tearing | Jeanne d'Arc's videos change the picture immediately. The picture tore when the plugin held that change back; it does not now |
| No missing pieces | Jeanne d'Arc's videos, two clips: no pieces of lines missing  |
| Cost | 2.7 to 3.0 ms a frame in Ys, 3.9 ms in Jeanne d'Arc, read from the label |
| Graphics chip figure | `GE 0%` read from the label in every game tried, with test builds that could not tell "asked and idle" from "not asked" or "call failed". The release shows `--`, `ERR` or `N/A` for those; which one each game shows has not been reported yet |
| Label | Steady in the top right corner, both lines |
| Strength and spacing buttons | 25% and 12.5% used; 1 in 4 used |
| Mode 2 | In the 59 fps game where the label's top line was cut in mode 1, switching to mode 2 cured it  |
| Label | `MODE 1` / `MODE 2` shown; small `%` and `MS` after the numbers |
| With MissyHUD | Both show at the same time |
| ARK's launcher | Lines and label show there too |

**Not yet confirmed on a PSP**

| Area | Status |
|---|---|
| Per-game settings | Built and in use with nothing reported against it, but a game staying off, or keeping its strength, across a restart has not been explicitly checked |
| 32-bit (8888) games | Passes its PC tests; no such game has been tried |
| The other strengths and 1 in 3 | Not walked through one by one |
| Mode 2's side effects | Tearing or sound popping in mode 2 is expected in some games from how it works; none has been reported, and no game has been checked for it on purpose |
| A game where the graphics chip is still drawing | The wait for it has only run in the emulator; every game tried read 0% |
| PS1 games, other PSP models | Not tried |

**Tested on a PC only**

- 53 checks of the pixel work built with AddressSanitizer: every strength in every pixel format and spacing, exact in every colour channel; nothing outside the dark rows touched; no frame darkened twice (repeat frames, a HUD redrawn on a still picture, partial redraws, triple buffering); dark noisy video losing no pieces; the settings files.
- 50 checks that run the compiled plugin in a MIPS emulator: attaching to the display call and detaching, the game's call still going through unchanged, frames from an interrupt handed to the plugin's thread, immediate changes passed on first, waiting for the graphics chip, and the label.

## Reporting a problem

Include the game, what you saw, and `ms0:/SCANLINE/scanline.log`. Its `built` line identifies the exact build. A photo of the label (hold L + R + UP, then L + R + DOWN) is useful too.
