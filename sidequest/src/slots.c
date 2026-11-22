/*
 * Sidequest - slot files. Each game has a folder ms0:/SIDEQUEST/<game>/ with
 * SUSPEND.SQS (slot 0) and STATE1..STATE5.SQS, each [4KB header][packed stream],
 * and a small picture next to each (.THM).
 * A slot is written to SLOTn.TMP and renamed only when complete, so a failed
 * save never replaces a good slot.
 */
#include <pspkernel.h>
#include <pspiofilemgr.h>
#include <string.h>
#include "common.h"
#include "slots.h"

#define CHUNK (256 * 1024)

#define PATH_LEN 128
static char g_dir[PATH_LEN] = SQ_DIR "/NOGAME";

/* Which game's folder the slot calls work on. `key` is a plain folder name. */
void slots_set_game(const char *key)
{
    sq_snprintf(g_dir, sizeof(g_dir), SQ_DIR "/%s", key);
}

static void slot_path(int k, char *out, const char *ext)
{
    if (k == 0) sq_snprintf(out, PATH_LEN, "%s/SUSPEND.%s", g_dir, ext);
    else        sq_snprintf(out, PATH_LEN, "%s/STATE%d.%s", g_dir, k, ext);
}

static u32 header_crc(const SqSlotHeader *h)
{
    return crc32_update(0, h, (u32)((u8 *)&h->header_crc - (u8 *)h));
}

int slot_write(int k, SqSlotHeader *h, const u8 *stream, u32 len, sq_progress_fn progress)
{
    char tmp[PATH_LEN], fin[PATH_LEN];
    slot_path(k, tmp, "TMP");
    slot_path(k, fin, "SQS");
    sceIoMkdir(SQ_DIR, 0777);
    sceIoMkdir(g_dir, 0777);

    h->stream_len = len;
    h->stream_crc = crc32_update(0, stream, len);
    h->header_crc = header_crc(h);

    SceUID fd = sceIoOpen(tmp, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd < 0) return -1;

    static u8 hdrbuf[SQ_HDR_SIZE] __attribute__((aligned(64)));
    memset(hdrbuf, 0, sizeof(hdrbuf));
    memcpy(hdrbuf, h, sizeof(*h));
    int ok = sceIoWrite(fd, hdrbuf, SQ_HDR_SIZE) == (int)SQ_HDR_SIZE;

    for (u32 off = 0; ok && off < len; off += CHUNK) {
        u32 n = len - off < CHUNK ? len - off : CHUNK;
        ok = sceIoWrite(fd, stream + off, n) == (int)n;
        if (progress) progress(off + n, len);
    }
    if (sceIoClose(fd) < 0) ok = 0;
    if (!ok) { sceIoRemove(tmp); return -2; }

    /* Swap the new file in. The old slot is only removed once the new one
     * has its final name; if the swap fails the old slot is put back. */
    char bak[PATH_LEN];
    slot_path(k, bak, "BAK");
    sceIoRemove(bak);
    int had_old = sceIoRename(fin, bak) >= 0;
    if (sceIoRename(tmp, fin) < 0) {
        if (had_old) sceIoRename(bak, fin);
        sceIoRemove(tmp);
        return -3;
    }
    if (had_old) sceIoRemove(bak);
    return 0;
}

int slot_read_header(int k, SqSlotHeader *h)
{
    char fin[PATH_LEN];
    slot_path(k, fin, "SQS");
    SceUID fd = sceIoOpen(fin, PSP_O_RDONLY, 0);
    if (fd < 0) return -1;
    int n = sceIoRead(fd, h, sizeof(*h));
    sceIoClose(fd);
    if (n != (int)sizeof(*h) || h->magic != SQ_MAGIC) return -2;
    if (h->version != SQ_HDR_VERSION || header_crc(h) != h->header_crc) return -3;
    /* strings come from a file: make sure they end */
    h->title[sizeof(h->title) - 1] = 0;
    h->disc_id[sizeof(h->disc_id) - 1] = 0;
    h->saved_at[sizeof(h->saved_at) - 1] = 0;
    h->boot_path[sizeof(h->boot_path) - 1] = 0;
    return 0;
}

/* Reads the packed stream into dst (at most cap bytes) and checks its CRC. */
int slot_read_stream(int k, const SqSlotHeader *h, u8 *dst, u32 cap, sq_progress_fn progress)
{
    char fin[PATH_LEN];
    if (h->stream_len > cap) return -4;
    slot_path(k, fin, "SQS");
    SceUID fd = sceIoOpen(fin, PSP_O_RDONLY, 0);
    if (fd < 0) return -1;
    if (sceIoLseek32(fd, SQ_HDR_SIZE, PSP_SEEK_SET) != (int)SQ_HDR_SIZE) { sceIoClose(fd); return -2; }

    u32 crc = 0;
    for (u32 off = 0; off < h->stream_len; off += CHUNK) {
        u32 n = h->stream_len - off < CHUNK ? h->stream_len - off : CHUNK;
        if (sceIoRead(fd, dst + off, n) != (int)n) { sceIoClose(fd); return -2; }
        crc = crc32_update(crc, dst + off, n);
        if (progress) progress(off + n, h->stream_len);
    }
    sceIoClose(fd);
    return crc == h->stream_crc ? 0 : -3;
}

int slot_delete(int k)
{
    char fin[PATH_LEN];
    slot_path(k, fin, "THM");
    sceIoRemove(fin);
    slot_path(k, fin, "BAK");
    sceIoRemove(fin);
    slot_path(k, fin, "SQS");
    int r = sceIoRemove(fin);
    sceIoRmdir(g_dir);                          /* only goes when it was the game's last file */
    return r;
}

/* SLOTn.THM: "SQT1", width, height, then width*height RGB565 pixels.
 * Separate from the slot file, so the slot format is unchanged and a
 * missing or broken picture never affects loading. */
typedef struct { char magic[4]; u16 w, h; } ThumbHead;

int slot_thumb_write(int k, const u16 *img, int w, int h)
{
    char fin[PATH_LEN];
    ThumbHead th = { { 'S', 'Q', 'T', '1' }, (u16)w, (u16)h };
    int bytes = w * h * 2;
    slot_path(k, fin, "THM");
    if (!img) return sceIoRemove(fin);          /* no picture: drop a stale one */
    SceUID fd = sceIoOpen(fin, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd < 0) return -1;
    int ok = sceIoWrite(fd, &th, sizeof(th)) == (int)sizeof(th) && sceIoWrite(fd, img, bytes) == bytes;
    sceIoClose(fd);
    if (!ok) sceIoRemove(fin);
    return ok ? 0 : -1;
}

int slot_thumb_read(int k, u16 *img, int w, int h)
{
    char fin[PATH_LEN];
    ThumbHead th;
    int bytes = w * h * 2;
    slot_path(k, fin, "THM");
    SceUID fd = sceIoOpen(fin, PSP_O_RDONLY, 0);
    if (fd < 0) return -1;
    int ok = sceIoRead(fd, &th, sizeof(th)) == (int)sizeof(th) && !memcmp(th.magic, "SQT1", 4) &&
             th.w == w && th.h == h && sceIoRead(fd, img, bytes) == bytes;
    sceIoClose(fd);
    return ok ? 0 : -1;
}

/* Open a slot's picture for reading row by row: returns the file, positioned
 * at the first pixel, and its size. The caller closes it. */
SceUID slot_thumb_open(int k, int *w, int *h)
{
    char fin[PATH_LEN];
    ThumbHead th;
    slot_path(k, fin, "THM");
    SceUID fd = sceIoOpen(fin, PSP_O_RDONLY, 0);
    if (fd < 0) return -1;
    if (sceIoRead(fd, &th, sizeof(th)) != (int)sizeof(th) || memcmp(th.magic, "SQT1", 4) ||
        th.w == 0 || th.h == 0 || th.w > 480 || th.h > 272) { sceIoClose(fd); return -1; }
    *w = th.w; *h = th.h;
    return fd;
}
