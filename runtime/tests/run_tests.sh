#!/bin/sh
# PC tests for Runtime's record logic (no PSP calls in track.c). Needs gcc.
set -e
cd "$(dirname "$0")"
mkdir -p run
gcc -g -fsanitize=address,undefined -Wall -Wextra -std=gnu99 -o run/test_track test_track.c ../src/track.c
./run/test_track
