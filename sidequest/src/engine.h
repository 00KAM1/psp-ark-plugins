#ifndef SQ_ENGINE_H
#define SQ_ENGINE_H
#include "common.h"

typedef struct { u32 r[13]; } SqCtx;

int  engine_init(void);
int  engine_ready(char *err, int errlen);       /* 0 if save/load can work here */
void engine_set_stage(u32 base_kseg0, u32 bytes);
u32  engine_stage_base(void);
u32  engine_me_addr(void);                      /* KSEG0, 0 if unknown */
u32  engine_me_size(void);
int  engine_me_in_lower(void);
void engine_set_regions(const SqRegion *r, int n);
int  engine_regions(SqRegion *out);             /* fills up to SQ_MAX_REGIONS, returns count */
int  engine_instant(void);                      /* 1 = no POWER press needed */
int  engine_self_wake(void);                    /* 1 = the PSP wakes itself after a save or load */

/* diagnostics */
int  engine_hook_count(void);
u32  engine_hook_hits(void);
u32  engine_resume_count(void);
int  engine_take_events(u32 *out, int max);
u32  engine_addr_orig(void);
u32  engine_addr_tag(void);

/*
 * Capture. Staging must be reserved and set. `on_wake` is called as early as
 * possible after the system resumes (used to freeze the game again).
 *   1  captured: *stream_len bytes are in staging, hdr is filled
 *   2  we just woke up inside a restored snapshot
 *  <0  error: -3 hook never ran, -4 the PSP never suspended, -5 does not fit
 */
int  engine_capture(SqSlotHeader *hdr, u32 *stream_len, void (*on_wake)(void));

/* Check a slot header against the running system. 0 = may be loaded. */
int  engine_validate(const SqSlotHeader *h, char *err, int errlen);

/* Swap to the snapshot whose packed stream is in staging. Returns only on
 * failure: -3 hook never ran, -4 never suspended, -6 staging changed. */
int  engine_restore(const SqSlotHeader *h, void (*on_wake)(void));

void engine_fill_identity(SqSlotHeader *h);
#endif
