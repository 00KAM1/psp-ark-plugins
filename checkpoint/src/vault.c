/*
 * Checkpoint - the vault: dated copies of save folders, and putting one back.
 *
 *   ms0:/PSP/SAVEDATA/<save>/            the game's live save
 *   ms0:/CHECKPOINT/<save>/<stamp>/       one copy per kept version
 *   ms0:/CHECKPOINT/<save>/<stamp>.PIN    empty marker: this version is pinned
 *
 * Rules that protect the player's data:
 *  - A copy is built in a NEW.TMP folder and renamed only when complete, so
 *    a half-written version never appears in the list.
 *  - Older versions are only removed after the new one is safely in place.
 *  - A restore first writes every file next to the live one as <name>.NEW.
 *    Only if all of them were written completely are the live files replaced.
 *    A failed restore leaves the live save exactly as it was.
 * Uses only sceIo*, so the same file is compiled and tested on a PC.
 */
#include <pspkernel.h>
#include <pspiofilemgr.h>
#include <string.h>
#include "common.h"

#define PATH_MAX_ 200
/* Work buffers. They used to be 32 KB of the plugin's own memory, held for the
 * whole game; kernel memory is short with several plugins loaded, and the
 * PSP's own save screen needs room there too. Now they are taken for the
 * length of one job and given back. If the system has none to give, two small
 * ones kept here do the job more slowly. */
#define BUF_BIG 16384
static u8  g_small[2048], g_small2[2048];
static u8 *g_buf = g_small, *g_buf2 = g_small2;
static int g_buf_size = sizeof(g_small);
static SceUID g_buf_id = -1;
static int g_buf_depth;

static void buf_take(void)
{
    if (g_buf_depth++) return;
    g_buf_id = sceKernelAllocPartitionMemory(PSP_MEMORY_PARTITION_KERNEL, "CpWork", PSP_SMEM_High, 2 * BUF_BIG, NULL);
    if (g_buf_id < 0) return;
    g_buf = sceKernelGetBlockHeadAddr(g_buf_id);
    g_buf2 = g_buf + BUF_BIG;
    g_buf_size = BUF_BIG;
}

static void buf_give(void)
{
    if (--g_buf_depth) return;
    g_buf = g_small; g_buf2 = g_small2; g_buf_size = sizeof(g_small);
    if (g_buf_id >= 0) sceKernelFreePartitionMemory(g_buf_id);
    g_buf_id = -1;
}

typedef struct { char name[BT_FILE_MAX]; u32 size; } FileEnt;

static int is_dir(const SceIoDirent *e) { return (e->d_stat.st_mode & FIO_S_IFDIR) != 0; }
static int dot(const char *n) { return !strcmp(n, ".") || !strcmp(n, ".."); }

/* Save folder names come from games and from folder listings. Only plain
 * names are accepted, so a path can never be steered outside the two roots. */
int vault_name_ok(const char *name)
{
    int n = 0;
    for (; name[n]; n++) {
        char c = name[n];
        int ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!ok || n >= BT_NAME_MAX - 1) return 0;
    }
    return n > 0;
}

static int stamp_ok(const char *s)
{
    for (int i = 0; i < BT_STAMP_LEN; i++) {
        if (i == 8) { if (s[i] != '_') return 0; }
        else if (s[i] < '0' || s[i] > '9') return 0;
    }
    return s[BT_STAMP_LEN] == 0;
}

static u32 num(const char *s, int n) { u32 v = 0; while (n--) v = v * 10 + (u32)(*s++ - '0'); return v; }

/* Minutes on a simplified calendar (every month 31 days). Only used to
 * order versions and to measure short gaps, where it is exact within a month. */
u32 vault_stamp_minutes(const char *s)
{
    if (!stamp_ok(s)) return 0;
    u32 days = (num(s, 4) - 2000) * 372 + (num(s + 4, 2) - 1) * 31 + (num(s + 6, 2) - 1);
    return (days * 24 + num(s + 9, 2)) * 60 + num(s + 11, 2);
}

/* Plain files of a folder. Returns the count, or BT_E_TOOMANY / BT_E_MISSING. */
static int list_files(const char *dir, FileEnt *out, int max)
{
    SceUID d = sceIoDopen(dir);
    if (d < 0) return BT_E_MISSING;
    int n = 0, r = 0;
    SceIoDirent e;
    for (;;) {
        memset(&e, 0, sizeof(e));
        if (sceIoDread(d, &e) <= 0) break;
        if (dot(e.d_name) || is_dir(&e)) continue;
        if (n >= max || strlen(e.d_name) >= BT_FILE_MAX) { r = BT_E_TOOMANY; break; }
        strcpy(out[n].name, e.d_name);
        out[n].size = (u32)e.d_stat.st_size;
        n++;
    }
    sceIoDclose(d);
    return r < 0 ? r : n;
}

u32 vault_dir_kb(const char *dir)
{
    static FileEnt f[BT_MAX_FILES];
    int n = list_files(dir, f, BT_MAX_FILES);
    u32 kb = 0;
    for (int i = 0; i < n; i++) kb += (f[i].size + 1023) / 1024;
    return kb;
}

static int copy_file_in(const char *src, const char *dst)
{
    SceUID in = sceIoOpen(src, PSP_O_RDONLY, 0);
    if (in < 0) return -1;
    SceUID out = sceIoOpen(dst, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (out < 0) { sceIoClose(in); return -1; }
    int r = 0;
    for (;;) {
        int n = sceIoRead(in, g_buf, g_buf_size);
        if (n < 0) { r = -1; break; }
        if (n == 0) break;
        if (sceIoWrite(out, g_buf, n) != n) { r = -1; break; }
    }
    sceIoClose(in);
    if (sceIoClose(out) < 0) r = -1;
    if (r < 0) sceIoRemove(dst);
    return r;
}

static int same_file_in(const char *a, const char *b)
{
    SceUID fa = sceIoOpen(a, PSP_O_RDONLY, 0);
    if (fa < 0) return 0;
    SceUID fb = sceIoOpen(b, PSP_O_RDONLY, 0);
    if (fb < 0) { sceIoClose(fa); return 0; }
    int same = 1;
    for (;;) {
        int na = sceIoRead(fa, g_buf, g_buf_size);
        int nb = sceIoRead(fb, g_buf2, g_buf_size);
        if (na != nb || na < 0 || memcmp(g_buf, g_buf2, na)) { same = 0; break; }
        if (na == 0) break;
    }
    sceIoClose(fa);
    sceIoClose(fb);
    return same;
}

static int copy_file(const char *src, const char *dst)
{
    buf_take();
    int r = copy_file_in(src, dst);
    buf_give();
    return r;
}

static int same_file(const char *a, const char *b)
{
    buf_take();
    int r = same_file_in(a, b);
    buf_give();
    return r;
}


/* Same file names, sizes and contents in both folders. */
static int same_dir(const char *a, const char *b)
{
    static FileEnt fa[BT_MAX_FILES], fb[BT_MAX_FILES];
    char pa[PATH_MAX_], pb[PATH_MAX_];
    int na = list_files(a, fa, BT_MAX_FILES), nb = list_files(b, fb, BT_MAX_FILES);
    if (na <= 0 || na != nb) return 0;
    for (int i = 0; i < na; i++) {
        int j = 0;
        while (j < nb && strcmp(fa[i].name, fb[j].name)) j++;
        if (j == nb || fa[i].size != fb[j].size) return 0;
    }
    for (int i = 0; i < na; i++) {
        sq_snprintf(pa, sizeof(pa), "%s/%s", a, fa[i].name);
        sq_snprintf(pb, sizeof(pb), "%s/%s", b, fa[i].name);
        if (!same_file(pa, pb)) return 0;
    }
    return 1;
}

/* Remove a folder that holds only files (a version, or a leftover NEW.TMP). */
static void remove_flat_dir(const char *dir)
{
    static FileEnt f[BT_MAX_FILES];
    char p[PATH_MAX_];
    for (int pass = 0; pass < 8; pass++) {              /* more files than one listing holds */
        int n = list_files(dir, f, BT_MAX_FILES);
        if (n == BT_E_MISSING) return;
        int count = n < 0 ? BT_MAX_FILES : n;
        if (n == BT_E_TOOMANY) {                        /* list_files stopped early: delete what it gave */
            SceUID d = sceIoDopen(dir);
            SceIoDirent e;
            count = 0;
            if (d >= 0) {
                for (;;) {
                    memset(&e, 0, sizeof(e));
                    if (sceIoDread(d, &e) <= 0 || count >= BT_MAX_FILES) break;
                    if (dot(e.d_name) || is_dir(&e) || strlen(e.d_name) >= BT_FILE_MAX) continue;
                    strcpy(f[count++].name, e.d_name);
                }
                sceIoDclose(d);
            }
        }
        for (int i = 0; i < count; i++) {
            sq_snprintf(p, sizeof(p), "%s/%s", dir, f[i].name);
            sceIoRemove(p);
        }
        if (n >= 0) break;
    }
    sceIoRmdir(dir);
}

/* Versions of one save, newest first. */
int vault_list_versions(const char *save, BtVersion *out, int max)
{
    char dir[PATH_MAX_];
    if (!vault_name_ok(save)) return 0;
    sq_snprintf(dir, sizeof(dir), BT_DIR "/%s", save);
    SceUID d = sceIoDopen(dir);
    if (d < 0) return 0;
    int n = 0;
    SceIoDirent e;
    for (;;) {
        memset(&e, 0, sizeof(e));
        if (sceIoDread(d, &e) <= 0) break;
        if (!is_dir(&e) || !stamp_ok(e.d_name)) continue;
        BtVersion v;
        strcpy(v.stamp, e.d_name);
        v.minutes = vault_stamp_minutes(e.d_name);
        int i = n < max ? n : max - 1;                  /* insertion sort, newest first; the oldest falls off */
        if (n >= max && strcmp(v.stamp, out[max - 1].stamp) <= 0) continue;
        while (i > 0 && strcmp(out[i - 1].stamp, v.stamp) < 0) { out[i] = out[i - 1]; i--; }
        out[i] = v;
        if (n < max) n++;
    }
    sceIoDclose(d);
    for (int i = 0; i < n; i++) {                       /* after closing: one directory call at a time */
        char pin[PATH_MAX_];
        SceIoStat st;
        sq_snprintf(pin, sizeof(pin), "%s/%s.PIN", dir, out[i].stamp);
        out[i].pinned = sceIoGetstat(pin, &st) >= 0;
    }
    return n;
}

/* Pin or unpin a version. A pinned version is never removed to make room
 * and never replaced by a newer save; it only goes when the player deletes it. */
int vault_set_pin(const char *save, const char *stamp, int on)
{
    static BtVersion ver[BT_MAX_VERSIONS];
    char pin[PATH_MAX_];
    if (!vault_name_ok(save) || !stamp_ok(stamp)) return BT_E_MISSING;
    int n = vault_list_versions(save, ver, BT_MAX_VERSIONS), found = -1, pins = 0;
    for (int i = 0; i < n; i++) {
        if (!strcmp(ver[i].stamp, stamp)) found = i;
        pins += ver[i].pinned;
    }
    if (found < 0) return BT_E_MISSING;
    sq_snprintf(pin, sizeof(pin), BT_DIR "/%s/%s.PIN", save, stamp);
    if (!on) { sceIoRemove(pin); return BT_OK; }
    if (ver[found].pinned) return BT_OK;
    if (pins >= BT_MAX_PINS) return BT_E_PINS;
    SceUID fd = sceIoOpen(pin, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd < 0) return BT_E_COPY;
    sceIoClose(fd);
    return BT_OK;
}

/* Save folders in the vault whose name starts with `prefix` and that hold at least one version. */
int vault_list_saves(const char *prefix, char names[][BT_NAME_MAX], int max)
{
    SceUID d = sceIoDopen(BT_DIR);
    if (d < 0) return 0;
    int n = 0, plen = (int)strlen(prefix);
    SceIoDirent e;
    BtVersion one;
    for (;;) {
        memset(&e, 0, sizeof(e));
        if (sceIoDread(d, &e) <= 0 || n >= max) break;
        if (!is_dir(&e) || dot(e.d_name) || !vault_name_ok(e.d_name)) continue;
        if (strncmp(e.d_name, prefix, plen)) continue;
        strcpy(names[n], e.d_name);
        n++;
    }
    sceIoDclose(d);
    int kept = 0;                                       /* listing versions re-enters the directory API: do it after closing */
    for (int i = 0; i < n; i++)
        if (vault_list_versions(names[i], &one, 1) > 0) {
            if (kept != i) memmove(names[kept], names[i], BT_NAME_MAX);
            kept++;
        }
    return kept;
}

/*
 * Copy the live save into the vault as version `stamp`.
 *   keep         how many versions to keep for this save
 *   min_minutes  if the newest existing version is younger than this, the new
 *                copy takes its place (frequent autosaves must not push the
 *                whole history out in ten minutes). Ignored when force is set.
 *   free_kb      free space on the stick in KB (0xFFFFFFFF = unknown)
 * Returns BT_OK, BT_SAME (identical to the newest version, nothing written),
 * BT_NOTHING (no live save), or a BT_E_ code.
 */
int vault_backup(const char *save, const char *stamp, int keep, int min_minutes, int force, u32 free_kb)
{
    static FileEnt f[BT_MAX_FILES];
    static BtVersion ver[BT_MAX_VERSIONS];
    char live[PATH_MAX_], base[PATH_MAX_], tmp[PATH_MAX_], fin[PATH_MAX_], a[PATH_MAX_], b[PATH_MAX_];

    if (!vault_name_ok(save) || !stamp_ok(stamp)) return BT_E_MISSING;
    sq_snprintf(live, sizeof(live), BT_SAVEROOT "/%s", save);
    sq_snprintf(base, sizeof(base), BT_DIR "/%s", save);
    sq_snprintf(tmp, sizeof(tmp), "%s/NEW.TMP", base);
    sq_snprintf(fin, sizeof(fin), "%s/%s", base, stamp);

    int n = list_files(live, f, BT_MAX_FILES);
    if (n == BT_E_MISSING || n == 0) return BT_NOTHING;
    if (n < 0) return n;

    int nver = vault_list_versions(save, ver, BT_MAX_VERSIONS);
    if (nver > 0) {
        sq_snprintf(a, sizeof(a), "%s/%s", base, ver[0].stamp);
        if (same_dir(live, a)) return BT_SAME;
    }

    u32 kb = 0;
    for (int i = 0; i < n; i++) kb += (f[i].size + 1023) / 1024;
    if (free_kb != 0xFFFFFFFFu && free_kb < kb + 4096) return BT_E_SPACE;   /* always leave 4MB for the game */

    sceIoMkdir(BT_DIR, 0777);
    sceIoMkdir(base, 0777);
    remove_flat_dir(tmp);                               /* leftover from an interrupted copy */
    if (sceIoMkdir(tmp, 0777) < 0) return BT_E_COPY;
    for (int i = 0; i < n; i++) {
        sq_snprintf(a, sizeof(a), "%s/%s", live, f[i].name);
        sq_snprintf(b, sizeof(b), "%s/%s", tmp, f[i].name);
        if (copy_file(a, b) < 0) { remove_flat_dir(tmp); return BT_E_COPY; }
    }
    remove_flat_dir(fin);                               /* two saves within the same second */
    sq_snprintf(a, sizeof(a), "%s.PIN", fin);
    sceIoRemove(a);
    if (sceIoRename(tmp, fin) < 0) { remove_flat_dir(tmp); return BT_E_COPY; }

    /* The new version is safe. Now thin out the old ones. Pinned versions
     * are left alone and do not count towards `keep`. */
    u32 now = vault_stamp_minutes(stamp);
    int first_old = 0;                                  /* ver[] still lists the versions from before this copy */
    if (nver > 0 && !strcmp(ver[0].stamp, stamp)) first_old = 1;
    if (!force && nver > first_old && !ver[first_old].pinned && now >= ver[first_old].minutes &&
        now - ver[first_old].minutes < (u32)min_minutes) {
        sq_snprintf(a, sizeof(a), "%s/%s", base, ver[first_old].stamp);
        remove_flat_dir(a);
        first_old++;
    }
    int kept = 1;                                       /* the new one */
    for (int i = first_old; i < nver; i++) {
        if (ver[i].pinned) continue;
        if (kept < keep) { kept++; continue; }
        sq_snprintf(a, sizeof(a), "%s/%s", base, ver[i].stamp);
        remove_flat_dir(a);
    }
    return BT_OK;
}

/* Put version `stamp` back as the live save. */
int vault_restore(const char *save, const char *stamp)
{
    static FileEnt f[BT_MAX_FILES], cur[BT_MAX_FILES];
    char live[PATH_MAX_], src[PATH_MAX_], a[PATH_MAX_], b[PATH_MAX_];

    if (!vault_name_ok(save) || !stamp_ok(stamp)) return BT_E_MISSING;
    sq_snprintf(live, sizeof(live), BT_SAVEROOT "/%s", save);
    sq_snprintf(src, sizeof(src), BT_DIR "/%s/%s", save, stamp);

    int n = list_files(src, f, BT_MAX_FILES);
    if (n <= 0) return n < 0 ? n : BT_E_MISSING;
    for (int i = 0; i < n; i++)
        if (strlen(f[i].name) + 4 >= BT_FILE_MAX) return BT_E_TOOMANY;

    sceIoMkdir(BT_SAVEROOT, 0777);
    sceIoMkdir(live, 0777);

    /* 1. every file as <name>.NEW next to the live one */
    int ok = 1;
    for (int i = 0; i < n && ok; i++) {
        sq_snprintf(a, sizeof(a), "%s/%s", src, f[i].name);
        sq_snprintf(b, sizeof(b), "%s/%s.NEW", live, f[i].name);
        if (copy_file(a, b) < 0) ok = 0;
    }
    if (!ok) {                                          /* undo: the live save was not touched */
        for (int i = 0; i < n; i++) {
            sq_snprintf(b, sizeof(b), "%s/%s.NEW", live, f[i].name);
            sceIoRemove(b);
        }
        return BT_E_COPY;
    }

    /* 2. drop live files that the version does not have */
    int ncur = list_files(live, cur, BT_MAX_FILES);
    for (int i = 0; i < ncur; i++) {
        int len = (int)strlen(cur[i].name), keepit = 0;
        if (len > 4 && !strcmp(cur[i].name + len - 4, ".NEW")) continue;
        for (int j = 0; j < n; j++) if (!strcmp(cur[i].name, f[j].name)) keepit = 1;
        if (!keepit) {
            sq_snprintf(a, sizeof(a), "%s/%s", live, cur[i].name);
            sceIoRemove(a);
        }
    }

    /* 3. swap each file in */
    int r = BT_OK;
    for (int i = 0; i < n; i++) {
        sq_snprintf(a, sizeof(a), "%s/%s", live, f[i].name);
        sq_snprintf(b, sizeof(b), "%s/%s.NEW", live, f[i].name);
        sceIoRemove(a);
        if (sceIoRename(b, a) < 0) r = BT_E_COPY;
    }
    return r;
}

int vault_delete_version(const char *save, const char *stamp)
{
    char p[PATH_MAX_];
    if (!vault_name_ok(save) || !stamp_ok(stamp)) return BT_E_MISSING;
    sq_snprintf(p, sizeof(p), BT_DIR "/%s/%s.PIN", save, stamp);
    sceIoRemove(p);
    sq_snprintf(p, sizeof(p), BT_DIR "/%s/%s", save, stamp);
    remove_flat_dir(p);
    sq_snprintf(p, sizeof(p), BT_DIR "/%s", save);
    sceIoRmdir(p);                                      /* only succeeds when it was the last version */
    return BT_OK;
}

/* A text value from a PARAM.SFO (e.g. SAVEDATA_TITLE, SAVEDATA_DETAIL). */
/* raw = 1 keeps bytes above 0x7E as they are (the text is UTF-8); raw = 0
 * turns them into '?' for the 8x8 font. */
static int sfo_string_in(const char *sfo_path, const char *key, char *out, int len, int raw)
{
    out[0] = 0;
    SceUID fd = sceIoOpen(sfo_path, PSP_O_RDONLY, 0);
    if (fd < 0) return -1;
    int n = sceIoRead(fd, g_buf, g_buf_size);
    sceIoClose(fd);
    if (n < 20 || memcmp(g_buf, "\0PSF", 4)) return -1;
    u32 keys, data, count;
    memcpy(&keys, g_buf + 8, 4); memcpy(&data, g_buf + 12, 4); memcpy(&count, g_buf + 16, 4);
    if (count > 64 || keys >= (u32)n || data >= (u32)n) return -1;
    for (u32 i = 0; i < count; i++) {
        const u8 *e = g_buf + 20 + i * 16;
        if (e + 16 > g_buf + n) return -1;
        u16 koff, fmt; u32 used, doff;
        memcpy(&koff, e, 2); memcpy(&fmt, e + 2, 2); memcpy(&used, e + 4, 4); memcpy(&doff, e + 12, 4);
        if (keys + koff >= (u32)n) return -1;
        const char *k = (const char *)g_buf + keys + koff;
        u32 room = (u32)n - (keys + koff), kl = 0;
        while (kl < room && k[kl]) kl++;
        if (kl == room) return -1;                      /* key name runs off the end */
        if (strcmp(k, key)) continue;
        if ((fmt & 0xFF) != 0x04 || data + doff >= (u32)n) return -1;      /* text values only */
        u32 avail = (u32)n - (data + doff);
        if (used > avail) used = avail;
        int o = 0;
        for (u32 j = 0; j < used && o < len - 1; j++) {
            u8 c = g_buf[data + doff + j];
            if (!c) break;
            out[o++] = (c == '\r' || c == '\n' || c == '\t') ? ' ' : (c < 0x20 || c == 0x7F || (c > 0x7E && !raw)) ? '?' : (char)c;
        }
        out[o] = 0;
        return 0;
    }
    return -1;
}

static int sfo_string(const char *sfo_path, const char *key, char *out, int len, int raw)
{
    buf_take();
    int r = sfo_string_in(sfo_path, key, out, len, raw);
    buf_give();
    return r;
}

int vault_sfo_string(const char *sfo_path, const char *key, char *out, int len)
{
    return sfo_string(sfo_path, key, out, len, 0);
}

/* The text exactly as the game wrote it. May end in a cut-off character;
 * journey_clean() tidies that. */
int vault_sfo_text(const char *sfo_path, const char *key, char *out, int len)
{
    return sfo_string(sfo_path, key, out, len, 1);
}
