#ifndef SQ_STAGING_H
#define SQ_STAGING_H
#include <psptypes.h>
int   staging_supported_model(int model);
u32   staging_capacity(void);           /* valid after a successful staging_alloc() */
int   staging_alloc(char *err, int errlen);
void  staging_free(void);
void  staging_release_all(void);   /* give the work area back (game is exiting) */
void  staging_describe(void);
void *staging_ptr(void);
#endif
