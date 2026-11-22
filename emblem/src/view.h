#ifndef AC_VIEW_H
#define AC_VIEW_H
#include "em.h"
#include "ach.h"
/* view.c: the achievements viewer (L + R + LEFT), over the frozen game. */
void view_setup(void);
int  view_blocked(void);                        /* 1: another plugin has the game frozen and owns the screen */
void view_run(const EmGame *g, const AcSet *have, const char *file_name, int file_errors);
#endif
