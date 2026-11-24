# Runtime source

Time on and battery use for a PSP on ARK-5, counted in every program. Runtime is a companion to the [Shelf launcher](https://github.com/00KAM1/psp-shelf-launcher), which shows the numbers; on its own it shows nothing. User manual and internals: [../docs/runtime.md](../docs/runtime.md).

```
export PSPDEV=/path/to/pspdev
export PATH=$PSPDEV/bin:$PATH
make                    # gives runtime.prx
sh tests/run_tests.sh   # PC tests; needs gcc
```

| File | Purpose |
|---|---|
| `src/main.c` | Start-up, the thread, clock and battery readings, file I/O |
| `src/track.c`, `src/track.h` | What one reading does to the record; no PSP calls (tested on a PC) |
