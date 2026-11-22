#!/bin/sh
# Runs every test that does not need a PSP. Run from the project root after `make`.
set -e
cd "$(dirname "$0")/.."
mkdir -p tests/run
# the pure layout function is tested straight from the plugin source
{ echo '#include "../src/common.h"'; echo 'typedef struct { u32 lo, hi; } Range;'; awk '/^int staging_merge/,/^}/' src/staging.c; awk '/^u32 staging_largest_gap/,/^}/' src/staging.c; } > tests/layout.c
( cd tests/run && rm -rf ms0 && \
  gcc -std=gnu99 -O1 -Wall -fsanitize=address,undefined -I../shim -include ../shim/psptypes.h \
      -o test_host ../test_host.c ../host_shim.c ../../src/util.c ../../src/slots.c ../../src/compress.c ../layout.c && \
  ASAN_OPTIONS=detect_leaks=0 ./test_host && rm -rf ms0 test_host )
python3 tests/test_sim.py sidequest.elf
