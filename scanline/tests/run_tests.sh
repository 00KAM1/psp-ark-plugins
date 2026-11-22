#!/bin/sh
# Runs the tests that need no PSP. Run from anywhere.
set -e
cd "$(dirname "$0")"
mkdir -p run
( cd run && gcc -std=gnu99 -O1 -Wall -Wextra -Wno-unused-parameter -fsanitize=address,undefined \
      -I../shim -include ../shim/psptypes.h -o test_host ../test_host.c ../host_shim.c ../../src/lines.c ../../src/util.c && \
  ./test_host && rm -f test_host )
