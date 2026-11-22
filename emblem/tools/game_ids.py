#!/usr/bin/env python3
"""Prints the game ID of every PSP ISO in a folder (read from UMD_DATA.BIN), one per line:
    ULUS10558    Ys The Oath In Felghana (USA) [Undub].iso
usage: python3 game_ids.py ~/Documents/PSP_BAK/ISO/PSP ~/Documents/PSP_BAK/ISO/MINIS
Reads a few KB of each ISO; changes nothing."""
import os, struct, sys

def game_id(path):
    with open(path, "rb") as f:
        f.seek(16 * 2048)
        pvd = f.read(2048)
        if pvd[1:6] != b"CD001":
            return "?"
        lba, size = struct.unpack_from("<I", pvd, 156 + 2)[0], struct.unpack_from("<I", pvd, 156 + 10)[0]
        f.seek(lba * 2048)
        root = f.read(min(size, 64 * 2048))
        i = 0
        while i < len(root):
            n = root[i]
            if n == 0:                                   # rest of this sector is empty
                i = (i // 2048 + 1) * 2048
                continue
            name = root[i + 33:i + 33 + root[i + 32]].split(b";")[0].upper()
            if name == b"UMD_DATA.BIN":
                f.seek(struct.unpack_from("<I", root, i + 2)[0] * 2048)
                return f.read(32).split(b"|")[0].decode("ascii", "replace").replace("-", "")
            i += n
    return "?"

for folder in sys.argv[1:] or ["."]:
    for name in sorted(os.listdir(folder)):
        if name.lower().endswith(".iso"):
            try:
                print(f"{game_id(os.path.join(folder, name))}\t{name}")
            except OSError as e:
                print(f"?\t{name}\t({e})")
