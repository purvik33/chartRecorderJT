/* pdf.h - tiny dependency-free PDF writer.
 *
 * Emits multi-page PDF 1.4 documents using the two built-in base-14
 * fonts (Helvetica / Helvetica-Bold, WinAnsi), plus lines, filled and
 * stroked rectangles and polylines - enough for a data report with
 * tables and a vector trend chart. No image or font embedding, so the
 * output stays small and needs no external library.
 *
 * All coordinates are in points (1/72") and TOP-DOWN: y grows downward
 * from the top of the page, which keeps report layout code readable.
 * A4 portrait is 595x842, A4 landscape 842x595. */
#ifndef PDF_H
#define PDF_H

#include <stddef.h>

typedef struct pdf pdf_t;

pdf_t *pdf_new(void);
void   pdf_free(pdf_t *p);

/* start a new page of the given size; subsequent draws land on it */
void   pdf_page(pdf_t *p, float w, float h);
float  pdf_pw(pdf_t *p);   /* current page width  */
float  pdf_ph(pdf_t *p);   /* current page height */

/* text with its baseline at (x, y); colour is 0..255 RGB */
void pdf_text(pdf_t *p, float x, float y, float size, int bold,
              int r, int g, int b, const char *s);
/* text right-aligned so it ends at x */
void pdf_text_right(pdf_t *p, float x, float y, float size, int bold,
                    int r, int g, int b, const char *s);
/* rendered width of a string (points) */
float pdf_str_w(float size, int bold, const char *s);

void pdf_line(pdf_t *p, float x1, float y1, float x2, float y2,
              float lw, int gray);
void pdf_rect(pdf_t *p, float x, float y, float w, float h,
              float lw, int gray);              /* stroked outline   */
void pdf_fill(pdf_t *p, float x, float y, float w, float h,
              int r, int g, int b);             /* filled rectangle  */
/* polyline through n top-down points xy[0..2n-1] = x0,y0,x1,y1,... */
void pdf_polyline(pdf_t *p, const float *xy, int n, float lw,
                  int r, int g, int b);

int  pdf_save(pdf_t *p, const char *path);       /* 0 on success */

#endif
