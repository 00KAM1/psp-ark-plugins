# Emblem source

Local achievements for PSP games on ARK-5. User manual: [../docs/emblem.md](../docs/emblem.md). Writing a game file: [../docs/emblem-adding-games.md](../docs/emblem-adding-games.md). Internals: [../docs/emblem-how-it-works.md](../docs/emblem-how-it-works.md).

```
export PSPDEV=/path/to/pspdev PATH=$PSPDEV/bin:$PATH
make                          # gives emblem.prx
bash tests/run_tests.sh       # PC tests; needs gcc
gcc -O2 -std=gnu99 -Itests/shim -include tests/shim/psptypes.h -o /tmp/render_tool tools/render_tool.c src/draw.c
python3 tests/test_sim.py emblem.elf ../tools/msx.bin    # the compiled plugin in a MIPS emulator; pip install unicorn pyelftools
```

| Path | Purpose |
|---|---|
| `games/` | The game files, one per game ID. These go to `ms0:/EMBLEM/games/` |
| `src/em.c`, `src/em.h` | The rule engine: reads a game file, looks at the game's memory once a tick. No PSP calls |
| `src/main.c` | Start-up, finding the game, the control loop, unlocks and the queue of cards |
| `src/hook.c` | The display hook (Scanline's), drawing the card on each frame |
| `src/draw.c` | The card and the viewer's panel. No PSP calls |
| `src/view.c` | The viewer: freezing the game, input, the PS button and POWER |
| `src/ach.c` | The earned file |
| `tests/reference/` | Hand-written C rules for Felghana from an early test build, kept so the tests can compare the game file with them |
| `tools/` | Picture tools, the Felghana file's generator and `game_ids.py` |

Internal names use the prefix `ac_` / `AC_` from the plugin's working title.

Only link kernel imports that have already run on the PSP in one of these plugins; look anything else up at run time with `sctrlHENFindFunction`. A plugin that asks at load time for a function the system does not have is silently not loaded.
