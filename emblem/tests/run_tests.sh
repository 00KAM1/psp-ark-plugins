#!/bin/sh
# PC tests for Emblem. Run from anywhere.
set -e
cd "$(dirname "$0")"
mkdir -p run
CC="gcc -std=gnu99 -O1 -Wall -Wextra -Wno-unused-parameter -fsanitize=address,undefined -I../shim -include ../shim/psptypes.h"
( cd run && $CC -o test_engine ../test_engine.c ../../src/em.c && ./test_engine && rm -f test_engine )
( cd run && $CC -o test_files ../test_files.c ../host_shim.c ../../src/ach.c ../../src/draw.c && ./test_files && rm -f test_files )
