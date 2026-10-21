/* safari.h - shared browser pieces: main.c (browse shell), DOM renderer
 * (render.c), QuickJS engine (js.c). Transport lives in libk93 k93net. */
#ifndef SAFARI_H
#define SAFARI_H
#include "k93ui.h"
#include "k93html.h"
#include "k93net.h"
#include <stddef.h>

/* ---- fetch (main.c: thin wrapper over k93net) ---- */
/* GET url; returns malloc'd NUL-terminated body (binary bodies may contain
 * NUL; length via sf_fetch_len()), or NULL (err filled). */
char *sf_fetch(const char *url, int *status, char *err, size_t errlen);
size_t sf_fetch_len(void);

/* ---- page model ---- */
#define SF_MAXLINKS 256
#define SF_MAXIMG   24

typedef struct { char url[1280]; char text[128]; k93node *node; } sf_link;

/* canvas 2D surface: pixels owned by the page, drawn via js.c */
typedef struct {
    k93node *node;
    uint32_t *px;                      /* opaque RGB, row-major */
    int w, h;
    int has_rect;                      /* pending fill() rect */
    int rx, ry, rw, rh;
    int np;                            /* polyline points, max 64 */
    int ptx[64], pty[64];
    uint32_t prevcp;                   /* kerning pen */
} sf_cv;

typedef struct sf_page {
    ui_page pg;
    ui_pline *lbuf;
    ui_prun *rbuf;
    char *pool;                        /* run text storage */
    int lcap, rcap;
    int nruns;                        /* used slots in rbuf */
    size_t pcap, pused;
    sf_link links[SF_MAXLINKS];
    int nlinks;
    char base[1280];
    k93css *css;
    k93node *doc;                     /* DOM owning the lines */
    int width_px;
    struct { char url[1280]; uint32_t *px; int w, h; } img[SF_MAXIMG];
    int nimg;
    /* embedded documents (<iframe src>, <object> html): owned here so
     * link/JS node pointers into them stay valid until page free */
    #define SF_MAXDOC 8
    k93node *fdoc[SF_MAXDOC];
    int nfdoc;
    /* canvas 2D buffers keyed by canvas node (drawn by js.c, shown by
     * render.c; opaque white bg - no alpha compositing) */
    #define SF_MAXCV 8
    sf_cv cv[SF_MAXCV];
    int ncv;
} sf_page;

void sf_page_init(sf_page *p, int width_px);
void sf_page_free(sf_page *p);
void sf_render(sf_page *p, k93node *doc);   /* rebuild lines from DOM */
k93node *sf_body(k93node *doc);
const char *sf_title(k93node *doc, char *buf, size_t cap);
void sf_details_toggle(k93node *details);   /* flip the open attribute */

/* ---- js.c (QuickJS) ---- */
int  js_open(sf_page *p, k93node *doc, const char *base);
void js_close(void);
void js_run_scripts(k93node *doc);          /* in document order */
int  js_tick(void);                         /* timers+jobs; 1 = page dirty */
int  js_click(sf_link *l);   /* 0 none, 1 handled(navigate ok), 2 prevented */
const char *js_nav_url(void);
void js_nav_clear(void);
int  js_active(void);
int  js_run_url(const char *url);        /* javascript: URL; 1 = ran ok */

/* ---- stb_image (stbi.c) ---- */
unsigned char *stbi_load_from_memory(const unsigned char *buf, int len,
                                     int *x, int *y, int *comp, int req_comp);
void stbi_image_free(void *p);

#endif
