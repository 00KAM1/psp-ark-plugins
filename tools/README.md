# Tools

Run everything from the top of the repository. The pictures need `pip install Pillow`.

The manuals show photos of a PSP, not these pictures. The scripts below draw mock screens into `docs/images/` and are kept for checking a layout before it is built; their `.png` output is not kept in the repository.

`msx.bin` is the 8x8 font from the PSPSDK debug library, the same one all four plugins draw with.

## Sidequest and Checkpoint

`render_screens.py` draws the Sidequest and Checkpoint pictures in `docs/images/` from the plugins' own layout, colours, text and font. `render_journey.py` draws the journey log pages. The games behind the menus are stand-in scenes drawn by the script.

```
python3 tools/render_screens.py
python3 tools/render_journey.py
```

`make_jfont.py` (with `bdf.py`) builds `checkpoint/src/jfont_data.c` from the BDF file of Fusion Pixel Font 8px monospaced (Japanese), release 2026.11.10:

```
python3 tools/make_jfont.py fusion-pixel-8px-monospaced-ja.bdf checkpoint/src/jfont_data.c
```

## Scanline

`render_docs.py` draws the Scanline pictures in `docs/images/`. It runs a stand-in scene (drawn by the script, not a real game) through the plugin's own pixel code and label code, so the lines and the label are what the PSP shows.

```
gcc -O2 -std=gnu99 -Iscanline/tests/shim -include scanline/tests/shim/psptypes.h -o /tmp/render_tool tools/render_tool.c scanline/src/lines.c
python3 tools/render_docs.py /tmp/render_tool
```

`render_tool.c` is the small PC program behind it: it reads a 480x272 picture, runs `sl_frame()` and the label on it, and writes it back.

## Emblem

Emblem's tools are in `emblem/tools/` and are run from the `emblem` folder.

`render_docs.py` draws the Emblem pictures in `docs/images/` with the plugin's own drawing code and engine, over a stand-in scene, from the made-up game file `demo-game.txt`:

```
cd emblem
gcc -O2 -std=gnu99 -Itests/shim -include tests/shim/psptypes.h -o /tmp/emblem_card tools/render_tool.c src/draw.c
gcc -O2 -std=gnu99 -Itests/shim -include tests/shim/psptypes.h -o /tmp/emblem_view tools/render_view.c src/draw.c src/em.c
python3 tools/render_docs.py /tmp/emblem_card /tmp/emblem_view
```

- `make_felghana.py` writes `games/ULUS10558.txt` from the lists in `tests/reference/`.
- `game_ids.py` prints the game ID of every ISO in a folder.
