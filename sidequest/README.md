# Sidequest source

Whole-system save states for PSP games on ARK-5. User manual: [../docs/sidequest.md](../docs/sidequest.md). Internals: [../docs/sidequest-checkpoint-how-it-works.md](../docs/sidequest-checkpoint-how-it-works.md) and [DESIGN.md](DESIGN.md) (the first design notes; per-game slots and the self-wake came later and are in the technical notes).

```
export PSPDEV=/path/to/pspdev PATH=$PSPDEV/bin:$PATH
make                    # gives sidequest.prx
sh tests/run_tests.sh   # PC tests; needs gcc and Python with unicorn + pyelftools
```

| File | Purpose |
|---|---|
| `src/main.c` | Start-up, decides whether to stay loaded |
| `src/engine.c`, `src/engine_asm.S` | The resume hook, capture, restore, unpack stub, wake alarm |
| `src/staging.c` | Finds free and busy extra RAM |
| `src/compress.c` | Packing, unpacking, stream validation |
| `src/slots.c` | Slot and picture files, per-game folders |
| `src/ui.c` | Menu, continue prompt, suspend and quit, after-load settings |
| `src/blit.c` | Drawing and screen capture |
| `src/util.c` | Log, config, formatting |

New fields in `SqConfig` (`src/common.h`) must be added at the end: the simulator test writes to fixed offsets.
