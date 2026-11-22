#ifndef SQ_SLOTS_H
#define SQ_SLOTS_H
#include "common.h"
typedef void (*sq_progress_fn)(u32 done, u32 total);
int  slot_write(int k, SqSlotHeader *h, const u8 *stream, u32 len, sq_progress_fn progress);
int  slot_read_header(int k, SqSlotHeader *h);
int  slot_read_stream(int k, const SqSlotHeader *h, u8 *dst, u32 cap, sq_progress_fn progress);
int  slot_delete(int k);
void slots_set_game(const char *key);
#endif
