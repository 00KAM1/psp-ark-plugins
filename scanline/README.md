# Scanline source

CRT-style scanlines over PSP games on ARK-5. User manual: [../docs/scanline.md](../docs/scanline.md). Internals: [../docs/scanline-how-it-works.md](../docs/scanline-how-it-works.md).

```
export PSPDEV=/path/to/pspdev PATH=$PSPDEV/bin:$PATH
make                                      # gives scanline.prx
sh tests/run_tests.sh                     # PC tests; needs gcc
python3 tests/test_sim.py scanline.elf    # runs the compiled plugin in a MIPS emulator; pip install unicorn pyelftools
```

| File | Purpose |
|---|---|
| `src/main.c` | Start-up, buttons, label text, per-game settings at start and after a change |
| `src/hook.c` | The inline hook on `sceDisplaySetFrameBuf`, mode 1 / mode 2, the drawing thread for frames flipped from an interrupt, waiting for the graphics chip |
| `src/lines.c` | The pixel work, the guard against darkening a frame twice, the label (no PSP dependencies; tested on a PC) |
| `src/util.c` | Log, `config.txt`, `games.txt`, game names |

The Makefile builds with `-Werror`. Kernel functions that have not run on the PSP before are looked up at run time with `sctrlHENFindFunction`, never linked: a module that asks at load time for a function the system does not have is not loaded.
