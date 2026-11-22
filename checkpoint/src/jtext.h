/* Text that is not plain English: reading UTF-8 and finding 8x8 shapes for it. */
#ifndef JTEXT_H
#define JTEXT_H
extern const int            jfont_count;
extern const unsigned short jfont_cp[];
extern const unsigned char  jfont_bits[];
extern const int            jfont_fold_count;
extern const unsigned short jfont_fold_from[];
extern const unsigned short jfont_fold_to[];

unsigned jt_next(const char **s);               /* the next character of UTF-8 text; a bad byte gives '?' */
unsigned jt_fold(unsigned cp);                  /* half-width kana -> full width, accented letters -> plain */
const unsigned char *jt_glyph(unsigned cp);     /* 8 rows of 8 dots, or 0 if the font has no shape for it */
int  jt_cols(const char *s);                    /* how many 8-dot cells the text takes on screen */
int  jt_fit(char *dst, int size, const char *src, int cols);   /* whole characters only; returns cells used */
#endif
