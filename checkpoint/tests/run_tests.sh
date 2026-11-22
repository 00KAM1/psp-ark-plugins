#!/bin/sh
# PC tests for the file handling. Run from the project root.
set -e
cd "$(dirname "$0")"
mkdir -p run && cd run && rm -rf ms0
gcc -std=gnu99 -O1 -Wall -fsanitize=address,undefined -I../shim -include ../shim/psptypes.h \
    -o test_host ../test_host.c ../host_shim.c ../host_extra.c ../../src/vault.c ../../src/journey.c ../../src/jtext.c ../../src/jfont_data.c
ASAN_OPTIONS=detect_leaks=0 ./test_host
rm -rf ms0 test_host
