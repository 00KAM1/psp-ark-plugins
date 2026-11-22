#ifndef AC_ACH_H
#define AC_ACH_H
#include <psptypes.h>
#include "em.h"

/* What has been earned in one game: ms0:/EMBLEM/earned/<GAMEID>.txt, one line each,
 *   73 | 2026-11-22 14:03 | Smart Water | 10 pts                                  */
typedef struct {
    u8   has[EM_MAX_ID + 1];
    char date[EM_MAX_ID + 1][17];               /* "2026-11-22 14:03" */
} AcSet;
int  ac_count(const AcSet *set);
int  ac_load(const char *path, AcSet *set);     /* lines read; a missing file is 0 */
int  ac_append(const char *path, const char *game, const char *title, int id, const char *stamp,
               const char *ach_title, int points);  /* 0 = written; stamp YYYYMMDD_HHMMSS */
int  ac_copy_file(const char *from, const char *to);   /* 0 = copied */
#endif
