/*
 * Sidequest - whole-system suspend slots for PSP-2000/3000 on ARK-5.
 * Shared definitions.
 */
#ifndef SQ_COMMON_H
#define SQ_COMMON_H

#include <psptypes.h>

#define SQ_VERSION_STR   "1.1"
#define SQ_BUILD_STAMP   __DATE__ " " __TIME__

/* ---- Memory layout (64MB models) ----------------------------------------
 * 0x08000000..0x09FFFFFF  lower 32MB: kernel + VRAM save + game
 * 0x0A000000..0x0BFFFFFF  upper 32MB: extra RAM. The top 4MB is where the
 *                         Media Engine saves its memory while the PSP is
 *                         suspended, so it is part of every snapshot and is
 *                         never used as work space.
 * A snapshot = lower 32MB + ME save area. It is run-length packed into free
 * extra RAM ("staging") while the system is suspended.
 */
#define SQ_LOWER_PHYS    0x08000000u
#define SQ_LOWER_SIZE    0x02000000u
#define SQ_UPPER_PHYS    0x0A000000u
#define SQ_UPPER_END     0x0C000000u
#define SQ_RESETVEC      0xBFC00000u          /* 4KB SRAM shared with the ME */
#define SQ_RESETVEC_SIZE 0x1000u
#define SQ_STUB_SIZE     0x1000u              /* top of staging holds the swap stub */

#define KSEG0(p)         (((u32)(p) & 0x1FFFFFFFu) | 0x80000000u)
#define PHYS(a)          ((u32)(a) & 0x1FFFFFFFu)

/* ---- Files -------------------------------------------------------------- */
#define SQ_DIR           "ms0:/SIDEQUEST"
#define SQ_LOG_PATH      SQ_DIR "/sidequest.log"
#define SQ_CONFIG_PATH   SQ_DIR "/config.txt"
#define SQ_MAX_SLOTS     6       /* per game: slot 0 = suspend, 1..5 = states */
#define SQ_KEY_MAX       32      /* folder name of a game under ms0:/SIDEQUEST */

/* ---- Packed stream (see compress.c) ------------------------------------- */
#define SQ_T_LIT   0x00000000u   /* n words follow                           */
#define SQ_T_SEEK  0x40000000u   /* next word = destination address          */
#define SQ_T_FILL  0x80000000u   /* next word = value, repeated n times      */
#define SQ_T_END   0xC0000000u
#define SQ_T_MASK  0xC0000000u
#define SQ_N_MASK  0x3FFFFFFFu

/* ---- Slot file: [header, 4KB][packed stream] ---------------------------- */
#define SQ_MAGIC         0x31535153u          /* "SQS1" */
#define SQ_HDR_VERSION   5              /* 5 from 1.1: a state carries the flash re-scan; older states are refused */
#define SQ_MAX_REGIONS   8
#define SQ_HDR_SIZE      0x1000u

typedef struct {
    u32  magic;
    u32  version;
    u32  header_size;
    u32  stream_len;         /* bytes, multiple of 4                   */
    u32  stream_crc;
    u32  fw_version;         /* sceKernelDevkitVersion()               */
    u32  ark_version;
    u32  model;
    u32  nregions;           /* memory areas in the snapshot           */
    struct { u32 addr; u32 words; } regions[SQ_MAX_REGIONS];
    char title[128];
    char disc_id[16];
    char saved_at[32];
    char boot_path[128];     /* ISO / EBOOT the game was started from  */
    /* Addresses inside the snapshot, used to re-enter it. */
    u32  ctx_addr;
    u32  jumpctx_addr;
    u32  flag_addr;
    u32  header_crc;         /* crc of everything before this field    */
} SqSlotHeader;

#define SQ_RESTORED_MAGIC 0x52535444u

/* ---- Config ------------------------------------------------------------- */
typedef struct {
    int  enable_load;        /* 0 = saving only                              */
    int  hold_ms;
    int  deep_sleep;         /* 1 = use real sleep (POWER press) instead of
                                the instant suspend                           */
    u32  hotkey;
    int  test;               /* 0 = normal. 1 = X only suspends and resumes.
                                2 = X suspends, runs our hook, packs nothing.
                                3 = X packs a snapshot and unpacks it again at once.
                                4 = as 3, then also refreshes the Memory Stick. */
    int  resume_prompt;      /* 1 = offer to continue when a game with a slot starts */
    int  load_mute;          /* 1 = sound is muted after every load */
    int  load_brightness;    /* screen brightness in percent set after every load, 0 = leave it */
    int  wake_seconds;       /* the PSP wakes itself this many seconds after it sleeps for a
                                save or load (clock alarm). 0 = off, slide POWER as before */
    int  min_battery;        /* percent: no save or load at or below this on battery (0 = no limit) */
} SqConfig;

extern SqConfig g_cfg;
void config_load(void);

/* ---- flashfix.c: the internal flash after a load ------------------------- */
typedef struct {
    int ran, verify, lock, lists_bad, built, ok, result;
    u32 ms, base, changed, spares;
} FlashFix;
extern FlashFix g_ff;
int  ff_check(u32 *base_out);
int  ff_rescan(void);
void ff_report(void);
int  ff_detach(void);
int  ff_attach(void);

void sq_log(const char *fmt, ...);
int  sq_snprintf(char *buf, int size, const char *fmt, ...);
void sq_strcpy(char *dst, int size, const char *src);
u32  crc32_update(u32 crc, const void *data, u32 len);

/* ---- compress.c (no PSP dependencies) ----------------------------------- */
typedef struct { u32 addr; u32 words; } SqRegion;
u32 *sq_pack(const u32 *src, u32 words, u32 *dst, u32 *dst_end);
u32  sq_pack_measure(const u32 *src, u32 words);
int  sq_stream_validate(const u32 *s, u32 len_bytes, const SqRegion *regions, int nregions);

#endif
