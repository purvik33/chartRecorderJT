/* pdf.c - see pdf.h. Minimal PDF 1.4 writer (text + vector graphics). */
#include "pdf.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

typedef struct {
    float  w, h;
    char  *buf;
    size_t len, cap;
} page_t;

struct pdf {
    page_t *pages;
    int     npages, cap;
};

/* ---- Helvetica advance widths (per 1000 em) for common characters ---- */
static int helv_w(unsigned char c)
{
    if (c >= '0' && c <= '9') return 556;
    if (c >= 'A' && c <= 'Z') {
        switch (c) { case 'I': return 278; case 'J': return 500;
            case 'M': case 'W': return 889; case 'L': return 556;
            default: return 667; }
    }
    if (c >= 'a' && c <= 'z') {
        switch (c) { case 'i': case 'j': case 'l': return 222;
            case 'f': case 't': return 278; case 'm': case 'w': return 722;
            case 'r': return 333; default: return 500; }
    }
    switch (c) {
    case ' ': return 278; case '.': case ',': case ':': case ';':
    case '\'': case '|': case '!': return 278;
    case '-': return 333; case '/': case '\\': return 278;
    case '(': case ')': return 333; case '%': return 889;
    case 0xB0: return 400; /* degree */ case 0xB5: return 556; /* micro */
    default: return 556;
    }
}

float pdf_str_w(float size, int bold, const char *s)
{
    (void)bold;
    int w = 0;
    for (; s && *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c >= 0x80) {          /* skip UTF-8 continuation bytes */
            if ((c & 0xC0) == 0x80) continue;
        }
        w += helv_w(c);
    }
    return (float)w / 1000.0f * size;
}

static page_t *cur(pdf_t *p)
{
    return (p->npages > 0) ? &p->pages[p->npages - 1] : NULL;
}

static void emit(pdf_t *p, const char *fmt, ...)
{
    page_t *pg = cur(p);
    if (!pg) return;
    char tmp[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (pg->len + (size_t)n + 1 > pg->cap) {
        size_t nc = (pg->cap ? pg->cap : 4096);
        while (nc < pg->len + (size_t)n + 1) nc *= 2;
        char *nb = (char *)realloc(pg->buf, nc);
        if (!nb) return;
        pg->buf = nb; pg->cap = nc;
    }
    memcpy(pg->buf + pg->len, tmp, (size_t)n);
    pg->len += (size_t)n;
    pg->buf[pg->len] = 0;
}

/* write a PDF text string: UTF-8 -> WinAnsi (best effort) + escaping */
static void emit_str(pdf_t *p, const char *s)
{
    emit(p, "(");
    for (; s && *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == 0xC2 && (unsigned char)s[1]) {           /* U+00A0..00BF */
            unsigned char n = (unsigned char)s[1];
            if (n == 0xB0 || n == 0xB5 || n == 0xB2 || n == 0xB3) {
                emit(p, "\\%03o", n); s++; continue;
            }
        }
        if (c >= 0x80) {                                  /* other UTF-8   */
            if ((c & 0xC0) == 0xC0) { emit(p, "?");       /* lead byte     */ }
            continue;                                     /* drop the rest */
        }
        if (c == '(' || c == ')' || c == '\\') emit(p, "\\%c", c);
        else emit(p, "%c", c);
    }
    emit(p, ")");
}

pdf_t *pdf_new(void)
{
    pdf_t *p = (pdf_t *)calloc(1, sizeof(*p));
    return p;
}

void pdf_free(pdf_t *p)
{
    if (!p) return;
    for (int i = 0; i < p->npages; i++) free(p->pages[i].buf);
    free(p->pages);
    free(p);
}

void pdf_page(pdf_t *p, float w, float h)
{
    if (p->npages >= p->cap) {
        int nc = p->cap ? p->cap * 2 : 8;
        page_t *np = (page_t *)realloc(p->pages, (size_t)nc * sizeof(page_t));
        if (!np) return;
        p->pages = np; p->cap = nc;
    }
    page_t *pg = &p->pages[p->npages++];
    memset(pg, 0, sizeof(*pg));
    pg->w = w; pg->h = h;
}

float pdf_pw(pdf_t *p) { page_t *g = cur(p); return g ? g->w : 0; }
float pdf_ph(pdf_t *p) { page_t *g = cur(p); return g ? g->h : 0; }

void pdf_text(pdf_t *p, float x, float y, float size, int bold,
              int r, int g, int b, const char *s)
{
    page_t *pg = cur(p); if (!pg) return;
    emit(p, "BT /F%d %.1f Tf %.3f %.3f %.3f rg %.2f %.2f Td ",
         bold ? 2 : 1, size, r / 255.0, g / 255.0, b / 255.0,
         x, pg->h - y);
    emit_str(p, s);
    emit(p, " Tj ET\n");
}

void pdf_text_right(pdf_t *p, float x, float y, float size, int bold,
                    int r, int g, int b, const char *s)
{
    pdf_text(p, x - pdf_str_w(size, bold, s), y, size, bold, r, g, b, s);
}

void pdf_line(pdf_t *p, float x1, float y1, float x2, float y2,
              float lw, int gray)
{
    page_t *pg = cur(p); if (!pg) return;
    emit(p, "%.3f %.3f %.3f RG %.2f w %.2f %.2f m %.2f %.2f l S\n",
         gray / 255.0, gray / 255.0, gray / 255.0, lw,
         x1, pg->h - y1, x2, pg->h - y2);
}

void pdf_rect(pdf_t *p, float x, float y, float w, float h,
              float lw, int gray)
{
    page_t *pg = cur(p); if (!pg) return;
    emit(p, "%.3f %.3f %.3f RG %.2f w %.2f %.2f %.2f %.2f re S\n",
         gray / 255.0, gray / 255.0, gray / 255.0, lw,
         x, pg->h - y - h, w, h);
}

void pdf_fill(pdf_t *p, float x, float y, float w, float h,
              int r, int g, int b)
{
    page_t *pg = cur(p); if (!pg) return;
    emit(p, "%.3f %.3f %.3f rg %.2f %.2f %.2f %.2f re f\n",
         r / 255.0, g / 255.0, b / 255.0, x, pg->h - y - h, w, h);
}

void pdf_polyline(pdf_t *p, const float *xy, int n, float lw,
                  int r, int g, int b)
{
    page_t *pg = cur(p); if (!pg || n < 2) return;
    emit(p, "%.3f %.3f %.3f RG %.2f w ", r / 255.0, g / 255.0, b / 255.0, lw);
    for (int i = 0; i < n; i++)
        emit(p, "%.2f %.2f %c ", xy[2 * i], pg->h - xy[2 * i + 1],
             i ? 'l' : 'm');
    emit(p, "S\n");
}

int pdf_save(pdf_t *p, const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) return -1;

    int N = p->npages;
    int nobj = 4 + 2 * N;
    long *off = (long *)calloc((size_t)nobj + 1, sizeof(long));
    if (!off) { fclose(f); return -1; }

    fprintf(f, "%%PDF-1.4\n%%\xE2\xE3\xCF\xD3\n");

    /* 1: Catalog */
    off[1] = ftell(f);
    fprintf(f, "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n");

    /* 2: Pages */
    off[2] = ftell(f);
    fprintf(f, "2 0 obj\n<< /Type /Pages /Count %d /Kids [", N);
    for (int i = 0; i < N; i++) fprintf(f, "%d 0 R ", 5 + 2 * i);
    fprintf(f, "] >>\nendobj\n");

    /* 3,4: fonts */
    off[3] = ftell(f);
    fprintf(f, "3 0 obj\n<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica"
               " /Encoding /WinAnsiEncoding >>\nendobj\n");
    off[4] = ftell(f);
    fprintf(f, "4 0 obj\n<< /Type /Font /Subtype /Type1 /BaseFont"
               " /Helvetica-Bold /Encoding /WinAnsiEncoding >>\nendobj\n");

    for (int i = 0; i < N; i++) {
        page_t *pg = &p->pages[i];
        int po = 5 + 2 * i, co = 6 + 2 * i;
        off[po] = ftell(f);
        fprintf(f, "%d 0 obj\n<< /Type /Page /Parent 2 0 R"
                   " /MediaBox [0 0 %.0f %.0f]"
                   " /Resources << /Font << /F1 3 0 R /F2 4 0 R >> >>"
                   " /Contents %d 0 R >>\nendobj\n",
                po, pg->w, pg->h, co);
        off[co] = ftell(f);
        fprintf(f, "%d 0 obj\n<< /Length %lu >>\nstream\n",
                co, (unsigned long)pg->len);
        if (pg->len) fwrite(pg->buf, 1, pg->len, f);
        fprintf(f, "\nendstream\nendobj\n");
    }

    long xref = ftell(f);
    fprintf(f, "xref\n0 %d\n0000000000 65535 f \n", nobj + 1);
    for (int i = 1; i <= nobj; i++)
        fprintf(f, "%010ld 00000 n \n", off[i]);
    fprintf(f, "trailer\n<< /Size %d /Root 1 0 R >>\nstartxref\n%ld\n%%%%EOF\n",
            nobj + 1, xref);

    free(off);
    fclose(f);
    return 0;
}
