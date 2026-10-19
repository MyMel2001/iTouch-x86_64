/* render.c - DOM -> ui_page lines. CSS cascade (k93html) with specificity,
 * inline runs with per-run color/bold/link/font/underline, word wrap, <img>
 * fetch+decode via stb_image, svg (linked + inline) via k93svg with a small
 * per-page cache. */
#define _GNU_SOURCE
#include "safari.h"
#include "k93util.h"
#include "k93font.h"
#include "k93svg.h"
#include "stb_image.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define PAGE_INK   0x14202Eu
#define PAGE_SUB   0x6B7787u

#define SF_LCAP 4096
#define SF_RCAP 16384
#define SF_PCAP (512 * 1024)

struct seg { int off, len; uint32_t fg, bg; int bold; int link;
             int font; int underline; int strike; };
struct lwitem { int off, len; uint32_t fg, bg; int bold, link, font, deco; };

typedef struct {
    sf_page *p;
    uint32_t fg, bg;
    int bold, center, scale, hidden, in_pre, in_anchor;
    int indent;                        /* px */
    int font;                          /* k93font id from CSS, -1 = default */
    int underline, strike;
    int anchor_link;
    int ol_num;                        /* ordered-list counter */
    int xform;                         /* text-transform: 0 none 1 upper 2 lower */
    int op;                            /* opacity percent, 100 = opaque */
    int lh;                            /* line-height px, 0 = default */
    int width;                         /* css width px, 0 = auto */
    int bord;                          /* active box border width, 0 = none */
    uint32_t bordc;                    /* box border color */
    uint32_t segbg;                    /* run background (mark), 0 = none */
    int lstyle;                        /* K93L_* in effect for lists */
    int ltype;                         /* 0 none, 1 ul, 2 ol */
    char tbuf[8192];
    int tlen;
    struct seg segs[256];
    int nsegs;
} rctx;

void sf_page_init(sf_page *p, int width_px)
{
    memset(p, 0, sizeof *p);
    p->lcap = SF_LCAP;
    p->rcap = SF_RCAP;
    p->pcap = SF_PCAP;
    p->lbuf = calloc((size_t)p->lcap, sizeof *p->lbuf);
    p->rbuf = calloc((size_t)p->rcap, sizeof *p->rbuf);
    p->pool = malloc(p->pcap);
    p->width_px = width_px > 128 ? width_px : 640;
    p->pg.lines = p->lbuf;
    p->pg.cap = p->lcap;
}

void sf_page_free(sf_page *p)
{
    for (int i = 0; i < p->nimg; i++) free(p->img[i].px);
    p->nimg = 0;
    free(p->lbuf);
    free(p->rbuf);
    free(p->pool);
    memset(p, 0, sizeof *p);
}

k93node *sf_body(k93node *doc)
{
    k93node *b = k93html_query(doc, "body");
    return b ? b : doc;
}

const char *sf_title(k93node *doc, char *buf, size_t cap)
{
    k93node *t = k93html_query(doc, "title");
    buf[0] = 0;
    if (t) {
        char tmp[512];
        k93html_text_content(t, tmp, sizeof tmp);
        size_t o = 0;
        int sp = 1;
        for (size_t i = 0; tmp[i] && o + 1 < cap; i++) {
            if (isspace((unsigned char)tmp[i])) { if (sp) continue; sp = 1; }
            else sp = 0;
            buf[o++] = tmp[i];
        }
        buf[o] = 0;
    }
    return buf[0] ? buf : "Untitled";
}

/* --------------------------------------------------------- emitters */

static char *pool_put(sf_page *p, const char *s, int len)
{
    if (len < 0) len = 0;
    if (p->pused + (size_t)len + 1 >= p->pcap) return NULL;
    char *d = p->pool + p->pused;
    memcpy(d, s, (size_t)len);   /* UTF-8 passes through: TTF renders it */
    d[len] = 0;
    p->pused += (size_t)len + 1;
    return d;
}

static ui_pline *line_begin(sf_page *p, int scale, int center, int indent,
                            uint32_t bg)
{
    if (p->pg.n >= p->pg.cap) return NULL;
    ui_pline *l = &p->lbuf[p->pg.n++];
    memset(l, 0, sizeof *l);
    l->link = -1;
    l->scale = scale;
    l->center = center;
    l->indent = indent;
    l->bg = bg;
    l->runs = &p->rbuf[p->nruns];
    return l;
}

static void line_run(sf_page *p, ui_pline *l, const char *s, int len,
                     uint32_t fg, int bold, int link,
                     int font, int underline, uint32_t bg)
{
    if (!l || p->nruns >= p->rcap) return;
    char *t = pool_put(p, s, len);
    if (!t) return;
    ui_prun *r = &p->rbuf[p->nruns++];
    r->text = t;
    r->fg = fg;
    r->bold = bold;
    r->link = link;
    r->font = font;
    r->underline = underline;
    r->bg = bg;
    l->nruns++;
    if (link >= 0 && l->link < 0) l->link = link;
}

/* one emitted text line: optional border side bar + the wrapped segments */
static void emit_line(rctx *ctx, int scale, struct lwitem *lw, int nlw)
{
    sf_page *p = ctx->p;
    ui_pline *l = line_begin(p, scale, ctx->center, ctx->indent, ctx->bg);
    if (!l) return;
    l->lh = ctx->lh;
    if (ctx->bord)
        line_run(p, l, "| ", 2, ctx->bordc, 0, -1, -1, 0, 0);
    for (int k = 0; k < nlw; k++)
        line_run(p, l, ctx->tbuf + lw[k].off, lw[k].len,
                 lw[k].fg, lw[k].bold, lw[k].link, lw[k].font,
                 lw[k].deco, lw[k].bg);
}

/* horizontal rule line (border top/bottom, hr) */
static void emit_rule(rctx *ctx, char ch, uint32_t col)
{
    k93font_select(k93font_default_id());
    char s[2] = { ch, 0 };
    int cw = ui_text_width(1, s);
    int avail = ctx->p->width_px - ctx->indent - 12;
    if (avail < 64) avail = 64;
    int n = cw > 0 ? avail / cw : 0;
    if (n > 300) n = 300;
    if (n < 1) n = 1;
    char buf[301];
    memset(buf, ch, (size_t)n);
    buf[n] = 0;
    ui_pline *l = line_begin(ctx->p, 1, 0, ctx->indent, 0);
    if (l) line_run(ctx->p, l, buf, n, col, 0, -1, -1, 0, 0);
}

/* emit pending segments as wrapped line(s) */
static void flush(rctx *ctx, int force)
{
    sf_page *p = ctx->p;
    if (ctx->nsegs == 0) {
        if (force) line_begin(p, 1, 0, 0, 0);   /* blank spacer line */
        return;
    }
    int scale = ctx->scale < 1 ? 1 : (ctx->scale > 2 ? 2 : ctx->scale);
    int avail = p->width_px - ctx->indent - 12;
    if (ctx->width > 0 && ctx->width - 12 < avail) avail = ctx->width - 12;
    if (avail < 64) avail = 64;
    int basef = k93font_default_id();
    struct lwitem lw[256];
    int nlw = 0, lwpx = 0;
    for (int i = 0; i < ctx->nsegs; i++) {
        struct seg *s = &ctx->segs[i];
        k93font_select(s->font >= 0 ? s->font : basef);
        int off = s->off, left = s->len;
        while (left > 0) {
            const char *w = ctx->tbuf + off;
            int room = avail - lwpx;
            int ww = 0;                    /* measure only this segment's bytes */
            {
                char tmp[2] = { 0, 0 };
                for (int c = 0; c < left; c++) {
                    tmp[0] = w[c];
                    ww += ui_text_width(scale, tmp);
                }
            }
            if (ww <= room) {
                if (nlw < 256) {
                    lw[nlw].off = off; lw[nlw].len = left;
                    lw[nlw].fg = s->fg; lw[nlw].bg = s->bg;
                    lw[nlw].bold = s->bold;
                    lw[nlw].link = s->link;
                    lw[nlw].font = s->font;
                    lw[nlw].deco = s->underline | (s->strike << 1);
                    nlw++;
                }
                lwpx += ww;
                left = 0;
                break;
            }
            /* cut to fit: prefer last space */
            int cut = 0, px = 0, lastsp = -1;
            char tmp[2] = { 0, 0 };
            for (int c = 0; c < left; c++) {
                tmp[0] = w[c];
                px += ui_text_width(scale, tmp);
                if (px > room) break;
                cut = c + 1;
                if (w[c] == ' ') lastsp = cut;
            }
            if (cut <= 0) cut = 1;
            if (lastsp > 0 && lastsp < cut) cut = lastsp;
            if (nlw < 256) {
                lw[nlw].off = off; lw[nlw].len = cut;
                lw[nlw].fg = s->fg; lw[nlw].bg = s->bg;
                lw[nlw].bold = s->bold;
                lw[nlw].link = s->link;
                lw[nlw].font = s->font;
                lw[nlw].deco = s->underline | (s->strike << 1);
                nlw++;
            }
            off += cut;
            left -= cut;
            if (left > 0 && w[cut] == ' ') { off++; left--; }
            /* emit the full line */
            emit_line(ctx, scale, lw, nlw);
            nlw = 0;
            lwpx = 0;
        }
    }
    if (nlw > 0) emit_line(ctx, scale, lw, nlw);
    ctx->tlen = 0;
    ctx->nsegs = 0;
}

static void xform_put(rctx *ctx, const char *s, int len)
{
    char *d = ctx->tbuf + ctx->tlen;
    if (!ctx->xform) {
        memcpy(d, s, (size_t)len);
    } else {                                 /* ASCII-only case fold */
        for (int i = 0; i < len; i++) {
            char c = s[i];
            if (c >= 'a' && c <= 'z' && ctx->xform == 1) c = (char)(c - 32);
            else if (c >= 'A' && c <= 'Z' && ctx->xform == 2)
                c = (char)(c + 32);
            d[i] = c;
        }
    }
    ctx->tlen += len;
}

static void add_text(rctx *ctx, const char *s, int len)
{
    if (len <= 0) return;
    if (ctx->tlen + len + 1 >= (int)sizeof ctx->tbuf || ctx->nsegs >= 256)
        flush(ctx, 0);
    if (ctx->tlen + len + 1 >= (int)sizeof ctx->tbuf) return;
    if (ctx->nsegs > 0) {                    /* extend last seg if same attrs */
        struct seg *ls = &ctx->segs[ctx->nsegs - 1];
        if (ls->fg == ctx->fg && ls->bold == ctx->bold &&
            ls->link == (ctx->in_anchor ? ctx->anchor_link : -1) &&
            ls->font == ctx->font && ls->underline == ctx->underline &&
            ls->strike == ctx->strike && ls->bg == ctx->segbg) {
            xform_put(ctx, s, len);
            ls->len += len;
            return;
        }
    }
    struct seg *s2 = &ctx->segs[ctx->nsegs++];
    s2->off = ctx->tlen;
    s2->len = len;
    s2->fg = ctx->fg;
    s2->bg = ctx->segbg;
    s2->bold = ctx->bold;
    s2->link = ctx->in_anchor ? ctx->anchor_link : -1;
    s2->font = ctx->font;
    s2->underline = ctx->underline;
    s2->strike = ctx->strike;
    xform_put(ctx, s, len);
}

static void add_str(rctx *ctx, const char *s)
{
    add_text(ctx, s, (int)strlen(s));
}

/* collapsed-whitespace text (normal mode) */
static void add_text_collapse(rctx *ctx, const char *s)
{
    if (ctx->tlen == 0 && ctx->nsegs == 0)
        while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;
    int sp = 0;
    for (const char *q = s; *q; q++) {
        char c = *q;
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') { sp = 1; continue; }
        if (sp && ctx->tlen + 1 < (int)sizeof ctx->tbuf) add_text(ctx, " ", 1);
        sp = 0;
        add_text(ctx, &c, 1);
    }
    if (sp && ctx->tlen + 1 < (int)sizeof ctx->tbuf) add_text(ctx, " ", 1);
}

static int is_skip(const char *t)
{
    static const char *s[] = { "script", "style", "head", "meta", "link",
        "title", "noscript", "template", "base", "option", NULL };
    for (int i = 0; s[i]; i++) if (!strcmp(t, s[i])) return 1;
    return 0;
}

static int is_block(const char *t)
{
    static const char *b[] = { "p", "div", "section", "article", "header",
        "footer", "main", "aside", "figure", "figcaption", "blockquote",
        "address", "dt", "dd", "details", "summary", "h1", "h2", "h3", "h4",
        "h5", "h6", "ul", "ol", "table", "pre", "button", "dl", "form",
        "fieldset", "legend", "caption", NULL };
    for (int i = 0; b[i]; i++) if (!strcmp(t, b[i])) return 1;
    return 0;
}

static int link_register(sf_page *p, const char *url, k93node *n)
{
    if (p->nlinks >= SF_MAXLINKS) return -1;
    sf_link *l = &p->links[p->nlinks];
    k93_strlcpy(l->url, url ? url : "", sizeof l->url);
    l->node = n;
    char tmp[512];
    k93html_text_content(n, tmp, sizeof tmp);
    size_t o = 0;
    int sp = 1;
    for (size_t i = 0; tmp[i] && o + 1 < sizeof l->text; i++) {
        if (isspace((unsigned char)tmp[i])) { if (sp) continue; sp = 1; }
        else sp = 0;
        l->text[o++] = tmp[i];
    }
    l->text[o] = 0;
    return p->nlinks++;
}

/* body looks like SVG? (<?xml decl, comment, or <svg root - case-insens) */
static int looks_svg(const char *b, size_t n)
{
    if (n > 4096) n = 4096;
    for (size_t i = 0; i + 4 < n; i++)
        if (b[i] == '<' && tolower((unsigned char)b[i + 1]) == 's' &&
            tolower((unsigned char)b[i + 2]) == 'v' &&
            tolower((unsigned char)b[i + 3]) == 'g' &&
            !isalnum((unsigned char)b[i + 4]))
            return 1;
    return 0;
}

/* scale + composite over page white + cache + emit the image line; frees
 * the stb-allocated rgba buffer */
static void img_place(rctx *ctx, const char *key,
                      unsigned char *rgba, int w, int h)
{
    int maxw = ctx->p->width_px - ctx->indent - 16;
    if (maxw < 64) maxw = 64;
    int nw = w, nh = h;
    if (nw > maxw) { nh = h * maxw / w; nw = maxw; }
    if (nh > 400) { nw = nw * 400 / nh; nh = 400; }
    uint32_t *px = (nw > 0 && nh > 0)
        ? malloc((size_t)nw * (size_t)nh * 4) : NULL;
    if (px) {
        for (int yy = 0; yy < nh; yy++)
            for (int xx = 0; xx < nw; xx++) {
                int sx = xx * w / nw, sy = yy * h / nh;
                const unsigned char *s4 =
                    rgba + ((size_t)sy * w + sx) * 4;
                uint32_t a = s4[3];
                uint32_t c = ((uint32_t)s4[0] << 16) |
                             ((uint32_t)s4[1] << 8) | s4[2];
                if (a == 0) c = 0;
                else if (a < 255) {
                    uint32_t r = (((c >> 16) & 255) * a + 255 * (255 - a)) / 255;
                    uint32_t g = (((c >> 8) & 255) * a + 255 * (255 - a)) / 255;
                    uint32_t bl = ((c & 255) * a + 255 * (255 - a)) / 255;
                    c = (r << 16) | (g << 8) | bl;
                }
                px[(size_t)yy * nw + xx] = c | 0xFF000000u;
            }
    }
    stbi_image_free(rgba);
    if (!px) return;
    if (ctx->p->nimg < SF_MAXIMG) {
        int ii = ctx->p->nimg++;
        k93_strlcpy(ctx->p->img[ii].url, key, sizeof ctx->p->img[ii].url);
        ctx->p->img[ii].px = px;
        ctx->p->img[ii].w = nw;
        ctx->p->img[ii].h = nh;
        ui_pline *l = line_begin(ctx->p, 1, 0, ctx->indent, 0);
        if (l) {
            l->img = px;
            l->imgw = nw;
            l->imgh = nh;
        }
    } else {
        free(px);
    }
}

static void render_img(rctx *ctx, k93node *n)
{
    flush(ctx, 0);
    const char *src = k93html_attr(n, "src");
    const char *alt = k93html_attr(n, "alt");
    if (src && src[0]) {
        char abs[1280];
        url_absolutize(ctx->p->base, src, abs, sizeof abs);
        for (int i = 0; i < ctx->p->nimg; i++)   /* cache hit */
            if (!strcmp(ctx->p->img[i].url, abs)) {
                ui_pline *l = line_begin(ctx->p, 1, 0, ctx->indent, 0);
                if (l) {
                    l->img = ctx->p->img[i].px;
                    l->imgw = ctx->p->img[i].w;
                    l->imgh = ctx->p->img[i].h;
                }
                return;
            }
        int status = 0;
        char err[64] = "";
        char *body = sf_fetch(abs, &status, err, sizeof err);
        if (body && status >= 200 && status < 300 && ctx->p->nimg < SF_MAXIMG) {
            int w = 0, h = 0, comp = 0;
            size_t blen = sf_fetch_len();
            unsigned char *rgba = NULL;
            if (looks_svg(body, blen)) {
                /* svg -> png (own encoder) -> the shared stb decode path */
                size_t plen = 0;
                int pw = 0, ph = 0;
                uint8_t *png = k93svg_to_png(body, blen, &plen, &pw, &ph);
                if (png) {
                    rgba = stbi_load_from_memory(png, (int)plen,
                                                 &w, &h, &comp, 4);
                    k93_log("safari", "img(svg): url=%s png=%zu -> %dx%d %s",
                            abs, plen, w, h, rgba ? "decoded" : "fail");
                    free(png);
                }
            }
            if (!rgba)
                rgba = stbi_load_from_memory((const unsigned char *)body,
                                             (int)blen, &w, &h, &comp, 4);
            k93_log("safari", "img: url=%s status=%d len=%zu w=%d h=%d %s",
                    abs, status, blen, w, h,
                    rgba ? "decoded" : stbi_failure_reason());
            free(body);
            body = NULL;
            if (rgba && w > 0 && h > 0) img_place(ctx, abs, rgba, w, h);
        } else {
            k93_log("safari", "img: url=%s fetch-fail status=%d err=%s",
                    abs, status, err);
            free(body);
        }
    }
    if (alt && alt[0]) {
        add_str(ctx, "[");
        add_str(ctx, alt);
        add_str(ctx, "]");
    }
}

/* inline <svg> element: serialize the subtree back to svg text (attrs are
 * html-parser-lowercased; the svg mini-parser matches case-insensitively),
 * rasterize -> png -> the same stb decode path, cached under a content
 * hash so page rebuilds reuse the raster */
static void render_svg_inline(rctx *ctx, k93node *n)
{
    flush(ctx, 0);
    if (ctx->p->nimg >= SF_MAXIMG) return;
    size_t need = 64;
    for (int i = 0; i < n->nattr; i++)
        need += strlen(n->ak[i]) +
                2 * (n->av[i] ? strlen(n->av[i]) : 0) + 8;
    char *inner = k93html_inner_html(n);
    if (!inner) return;
    need += strlen(inner);
    char *s = malloc(need);
    if (!s) { free(inner); return; }
    size_t o = 0;
    o += (size_t)snprintf(s + o, need - o, "<svg");
    for (int i = 0; i < n->nattr; i++) {
        o += (size_t)snprintf(s + o, need - o, " %s=\"", n->ak[i]);
        for (const char *q = n->av[i] ? n->av[i] : ""; *q && o + 8 < need; q++) {
            const char *ins = NULL;
            if (*q == '&') ins = "&amp;";
            else if (*q == '<') ins = "&lt;";
            else if (*q == '>') ins = "&gt;";
            else if (*q == '"') ins = "&quot;";
            if (ins) o += (size_t)snprintf(s + o, need - o, "%s", ins);
            else s[o++] = *q;
        }
        s[o++] = '"';
    }
    o += (size_t)snprintf(s + o, need - o, ">%s</svg>", inner);
    free(inner);
    s[o] = 0;
    uint32_t hsh = 2166136261u;                  /* fnv-1a content key */
    for (size_t i = 0; i < o; i++) {
        hsh ^= (unsigned char)s[i];
        hsh *= 16777619u;
    }
    char key[32];
    snprintf(key, sizeof key, "svg:%08x", hsh);
    for (int i = 0; i < ctx->p->nimg; i++)
        if (!strcmp(ctx->p->img[i].url, key)) {
            free(s);
            ui_pline *l = line_begin(ctx->p, 1, 0, ctx->indent, 0);
            if (l) {
                l->img = ctx->p->img[i].px;
                l->imgw = ctx->p->img[i].w;
                l->imgh = ctx->p->img[i].h;
            }
            return;
        }
    size_t plen = 0;
    int w = 0, h = 0;
    uint8_t *png = k93svg_to_png(s, o, &plen, &w, &h);
    free(s);
    if (!png) return;
    int iw = 0, ih = 0, comp = 0;
    unsigned char *rgba = stbi_load_from_memory(png, (int)plen,
                                                &iw, &ih, &comp, 4);
    k93_log("safari", "svg-inline: key=%s png=%zu -> %dx%d %s",
            key, plen, iw, ih, rgba ? "decoded" : "fail");
    free(png);
    if (rgba && iw > 0 && ih > 0) img_place(ctx, key, rgba, iw, ih);
}

static void render_node(rctx *ctx, k93node *n);

/* ------------------------------------------------------- helpers ------ */

static uint32_t fade_to_white(uint32_t c, int op)  /* opacity blend, percent */
{
    if (op >= 100) return c;
    if (op < 0) op = 0;
    uint32_t r = (((c >> 16) & 255) * (uint32_t)op + 255u * (100u - (uint32_t)op)) / 100u;
    uint32_t g = (((c >> 8) & 255) * (uint32_t)op + 255u * (100u - (uint32_t)op)) / 100u;
    uint32_t b = ((c & 255) * (uint32_t)op + 255u * (100u - (uint32_t)op)) / 100u;
    return (r << 16) | (g << 8) | b;
}

static void collapse_ws(char *s)
{
    int sp = 0;
    size_t o = 0;
    for (size_t i = 0; s[i]; i++) {
        if (isspace((unsigned char)s[i])) { sp = o > 0; continue; }
        if (sp) s[o++] = ' ';
        sp = 0;
        s[o++] = s[i];
    }
    s[o] = 0;
}

/* measure with the default page font (selects it first) */
static int mw(const char *s)
{
    k93font_select(k93font_default_id());
    return ui_text_width(1, s);
}

/* ul/ol list markers honoring list-style-type */
static void list_marker(char *buf, size_t cap, int style, int ordered, int num)
{
    if (!ordered) {
        if (style == K93L_CIRCLE) snprintf(buf, cap, "o ");
        else if (style == K93L_SQUARE) snprintf(buf, cap, "# ");
        else if (style == K93L_NONE) { buf[0] = 0; return; }
        else snprintf(buf, cap, "- ");
        return;
    }
    if (style == K93L_LALPHA || style == K93L_UALPHA) {
        char tmp[8];
        int i = 0, n = num < 1 ? 1 : num;
        int upper = style == K93L_UALPHA;
        do {
            if (i < 8) tmp[i++] = (char)((upper ? 'A' : 'a') + (n - 1) % 26);
            n = (n - 1) / 26;
        } while (n > 0 && i < 8);
        size_t o = 0;
        while (i > 0 && o + 4 < cap) buf[o++] = tmp[--i];
        snprintf(buf + o, cap - o, ". ");
        return;
    }
    if (style == K93L_LROMAN || style == K93L_UROMAN) {
        static const struct { int v; const char *l; } rn[] = {
            { 1000, "m" }, { 900, "cm" }, { 500, "d" }, { 400, "cd" },
            { 100, "c" }, { 90, "xc" }, { 50, "l" }, { 40, "xl" },
            { 10, "x" }, { 9, "ix" }, { 5, "v" }, { 4, "iv" }, { 1, "i" },
            { 0, NULL }
        };
        char tmp[20];
        size_t o = 0;
        int n = num;
        if (n >= 1 && n <= 3999) {
            for (int i = 0; rn[i].v && o + 3 < sizeof tmp; i++)
                while (n >= rn[i].v && o + 3 < sizeof tmp) {
                    size_t sl = strlen(rn[i].l);
                    memcpy(tmp + o, rn[i].l, sl);
                    o += sl;
                    n -= rn[i].v;
                }
            tmp[o] = 0;
            if (style == K93L_UROMAN)
                for (size_t i = 0; tmp[i]; i++)
                    tmp[i] = (char)toupper((unsigned char)tmp[i]);
            snprintf(buf, cap, "%s. ", tmp);
            return;
        }
    }
    snprintf(buf, cap, "%d. ", num);        /* decimal (also roman fallback) */
}

/* [####----] NN% meter line (progress / meter) */
static void render_bar(rctx *ctx, int pct)
{
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    char bar[16];
    int w = 12, on = pct * w / 100;
    for (int i = 0; i < w; i++) bar[i] = i < on ? '#' : '-';
    bar[w] = 0;
    char out[32];
    snprintf(out, sizeof out, "[%s] %d%%", bar, pct);
    add_str(ctx, out);
}

/* one form control as an activatable line (click routed to JS default
 * action: toggle / native editor / option menu) */
static void render_widget(rctx *ctx, k93node *n)
{
    flush(ctx, 0);
    const char *typea = k93html_attr(n, "type");
    char type[24];
    k93_strlcpy(type, typea ? typea : "text", sizeof type);
    for (char *q = type; *q; q++) *q = (char)tolower((unsigned char)*q);
    const char *val = k93html_attr(n, "value");
    const char *ph = k93html_attr(n, "placeholder");
    const char *name = k93html_attr(n, "name");
    const char *label = (name && *name) ? name : (ph && *ph) ? ph : type;

    if (!strcmp(type, "hidden")) return;
    if (!strcmp(type, "checkbox") || !strcmp(type, "radio")) {
        int on = k93html_attr(n, "checked") != NULL;
        int idx = link_register(ctx->p, "", n);
        if (idx >= 0) { ctx->in_anchor = 1; ctx->anchor_link = idx; }
        add_str(ctx, !strcmp(type, "radio") ? (on ? "(o) " : "( ) ")
                                            : (on ? "[x] " : "[ ] "));
        add_str(ctx, label);
        ctx->in_anchor = 0;
        ctx->anchor_link = -1;
        flush(ctx, 0);
        return;
    }
    if (!strcmp(type, "submit") || !strcmp(type, "reset") ||
        !strcmp(type, "button")) {
        const char *lab = (val && *val) ? val
            : !strcmp(type, "submit") ? "Submit"
            : !strcmp(type, "reset") ? "Reset" : "Button";
        int idx = link_register(ctx->p, "", n);
        if (idx >= 0) { ctx->in_anchor = 1; ctx->anchor_link = idx; }
        int ob = ctx->bold;
        uint32_t osg = ctx->segbg;
        ctx->bold = 1;
        ctx->segbg = 0xE9EEF4;
        add_str(ctx, "[ ");
        add_str(ctx, lab);
        add_str(ctx, " ]");
        ctx->bold = ob;
        ctx->segbg = osg;
        ctx->in_anchor = 0;
        ctx->anchor_link = -1;
        flush(ctx, 0);
        return;
    }
    if (!strcmp(type, "image") || !strcmp(type, "file")) {
        const char *alt = k93html_attr(n, "alt");
        add_str(ctx, "[ ");
        add_str(ctx, (alt && *alt) ? alt : label);
        add_str(ctx, " ]");
        flush(ctx, 0);
        return;
    }
    /* text-ish field: text password search email url tel number date time */
    int idx = link_register(ctx->p, "", n);
    if (idx >= 0) { ctx->in_anchor = 1; ctx->anchor_link = idx; }
    int shown_val = val && *val;
    const char *shown = shown_val ? val : (ph && *ph ? ph : "");
    uint32_t ogf = ctx->fg, osg = ctx->segbg;
    if (!shown_val) ctx->fg = PAGE_SUB;
    ctx->segbg = 0xF1F4F8;
    add_str(ctx, "[ ");
    if (!strcmp(type, "password")) {
        char stars[64];
        size_t nl = strlen(shown);
        if (nl > 40) nl = 40;
        memset(stars, '*', nl);
        stars[nl] = 0;
        add_str(ctx, stars);
    } else {
        add_str(ctx, *shown ? shown : " ");
    }
    add_str(ctx, " ]");
    ctx->fg = ogf;
    ctx->segbg = osg;
    ctx->in_anchor = 0;
    ctx->anchor_link = -1;
    flush(ctx, 0);
}

/* <select>: chosen option + arrow; click opens the native option menu */
static void render_select(rctx *ctx, k93node *n)
{
    flush(ctx, 0);
    k93node *opt = NULL, *first = NULL;
    for (k93node *c = n->first; c; c = c->next) {
        k93node *group = NULL;
        if (!strcmp(c->tag, "option")) group = c;
        else if (!strcmp(c->tag, "optgroup")) group = NULL;
        if (group || !strcmp(c->tag, "option")) {
            if (!first) first = group;
            if (k93html_attr(group, "selected")) opt = group;
        } else if (!strcmp(c->tag, "optgroup")) {
            for (k93node *o = c->first; o; o = o->next)
                if (!strcmp(o->tag, "option")) {
                    if (!first) first = o;
                    if (k93html_attr(o, "selected")) opt = o;
                }
        }
    }
    if (!opt) opt = first;
    char txt[128] = "";
    if (opt) k93html_text_content(opt, txt, sizeof txt);
    collapse_ws(txt);
    int idx = link_register(ctx->p, "", n);
    if (idx >= 0) { ctx->in_anchor = 1; ctx->anchor_link = idx; }
    ctx->segbg = 0xF1F4F8;
    add_str(ctx, "[ ");
    add_str(ctx, txt[0] ? txt : " ");
    add_str(ctx, " \xE2\x96\xBC ]");         /* U+25BC down triangle */
    ctx->segbg = 0;
    ctx->in_anchor = 0;
    ctx->anchor_link = -1;
    flush(ctx, 0);
}

/* table: measured columns, ASCII grid, header underline; single-line cells
 * (ceiling: no colspan / no in-cell wrap) */
static void render_table(rctx *ctx, k93node *n)
{
    enum { TC = 8, TROWS = 48 };
    flush(ctx, 0);
    k93node *rows[TROWS];
    int nr = 0;
    char cap[256];
    cap[0] = 0;
    for (k93node *c = n->first; c && nr < TROWS; c = c->next) {
        if (!strcmp(c->tag, "caption")) {
            if (!cap[0]) {
                k93html_text_content(c, cap, sizeof cap);
                collapse_ws(cap);
            }
        } else if (!strcmp(c->tag, "tr")) {
            rows[nr++] = c;
        } else if (!strcmp(c->tag, "thead") || !strcmp(c->tag, "tbody") ||
                   !strcmp(c->tag, "tfoot")) {
            for (k93node *r = c->first; r && nr < TROWS; r = r->next)
                if (!strcmp(r->tag, "tr")) rows[nr++] = r;
        }
    }
    if (cap[0]) {
        add_str(ctx, cap);
        flush(ctx, 0);
    }
    if (!nr) return;

    char cell[TROWS][TC][80];
    int thf[TROWS][TC], nc[TROWS], ncmax = 0;
    memset(cell, 0, sizeof cell);
    memset(thf, 0, sizeof thf);
    for (int r = 0; r < nr; r++) {
        nc[r] = 0;
        for (k93node *c = rows[r]->first; c && nc[r] < TC; c = c->next) {
            if (strcmp(c->tag, "td") && strcmp(c->tag, "th")) continue;
            k93html_text_content(c, cell[r][nc[r]], sizeof cell[r][0]);
            collapse_ws(cell[r][nc[r]]);
            thf[r][nc[r]] = !strcmp(c->tag, "th");
            nc[r]++;
        }
        if (nc[r] > ncmax) ncmax = nc[r];
    }
    if (!ncmax) return;

    int avail = ctx->p->width_px - ctx->indent - 16;
    if (avail < 120) avail = 120;
    int barw = mw("| ");
    int dotw = mw("..");
    int maxcol = (avail - barw * (ncmax + 1)) / ncmax;
    if (maxcol < 32) maxcol = 32;
    int colw[TC];
    for (int c = 0; c < ncmax; c++) {
        int w = 0;
        for (int r = 0; r < nr; r++)
            if (c < nc[r]) {
                int cw = mw(cell[r][c]);
                if (cw > w) w = cw;
            }
        colw[c] = w > maxcol ? maxcol : w;
    }
    for (int r = 0; r < nr; r++)
        for (int c = 0; c < nc[r]; c++) {
            int trunc = 0;
            size_t len = strlen(cell[r][c]);
            while (len > 1 && mw(cell[r][c]) > colw[c] - dotw) {
                cell[r][c][--len] = 0;
                trunc = 1;
            }
            if (trunc && len + 3 < sizeof cell[r][0])
                strcat(cell[r][c], "..");
        }
    int total = barw;
    for (int c = 0; c < ncmax; c++) total += colw[c] + barw;

    int dw = mw("-");
    int dn = dw > 0 ? total / dw : 0;
    if (dn > 300) dn = 300;
    char rule[301];
    memset(rule, '-', (size_t)dn);
    rule[dn] = 0;
#define TABLE_RULE(col) do { \
        ui_pline *rl = line_begin(ctx->p, 1, 0, ctx->indent, 0); \
        if (rl) line_run(ctx->p, rl, rule, dn, (col), 0, -1, -1, 0, 0); \
    } while (0)

    TABLE_RULE(PAGE_SUB);                     /* top rule */
    int hdone = 0;
    for (int r = 0; r < nr; r++) {
        char row[768];
        size_t o = 0;
        row[o++] = '|';
        row[o++] = ' ';
        row[o] = 0;
        int target = barw;
        for (int c = 0; c < ncmax; c++) {
            if (c < nc[r]) {
                size_t cl = strlen(cell[r][c]);
                if (o + cl + 4 < sizeof row) {
                    memcpy(row + o, cell[r][c], cl);
                    o += cl;
                    row[o] = 0;
                }
            }
            target += colw[c];
            while (mw(row) < target && o + 4 < sizeof row) {
                row[o++] = ' ';
                row[o] = 0;
            }
        }
        if (o + 2 < sizeof row) {
            row[o++] = '|';
            row[o] = 0;
        }
        int any_th = 0;
        for (int c = 0; c < nc[r]; c++)
            if (thf[r][c]) any_th = 1;
        ui_pline *l = line_begin(ctx->p, 1, 0, ctx->indent, 0);
        if (l)
            line_run(ctx->p, l, row, (int)o, any_th ? 0x0B1B2Bu : PAGE_INK,
                     any_th, -1, -1, 0, 0);
        if (any_th && !hdone) {
            hdone = 1;
            TABLE_RULE(PAGE_SUB);             /* header underline */
        }
    }
    TABLE_RULE(PAGE_SUB);                     /* bottom rule */
#undef TABLE_RULE
    flush(ctx, 0);
}

static void render_node(rctx *ctx, k93node *n);

static void render_children(rctx *ctx, k93node *n)
{
    for (k93node *c = n->first; c; c = c->next) render_node(ctx, c);
}

static void render_node(rctx *ctx, k93node *n)
{
    if (!strcmp(n->tag, "#text")) {
        if (ctx->in_pre) {
            for (char *q = n->text; q && *q; q++) {
                if (*q == '\n') flush(ctx, 1);
                else {
                    char c = *q == '\t' ? ' ' : *q;
                    add_text(ctx, &c, 1);
                }
            }
        } else {
            add_text_collapse(ctx, n->text ? n->text : "");
        }
        return;
    }
    if (n->tag[0] == '#') return;
    const char *t = n->tag;
    if (is_skip(t)) return;

    /* cascade */
    uint32_t sf_fg = ctx->fg, sf_bg = ctx->bg;
    int sf_bold = ctx->bold, sf_center = ctx->center, sf_scale = ctx->scale;
    int sf_hidden = ctx->hidden, sf_indent = ctx->indent;
    int sf_font = ctx->font, sf_underline = ctx->underline;
    int sf_strike = ctx->strike, sf_xform = ctx->xform;
    int sf_op = ctx->op, sf_lh = ctx->lh, sf_width = ctx->width;
    int sf_lstyle = ctx->lstyle;
    uint32_t sf_segbg = ctx->segbg;
    k93cstyle st;
    memset(&st, 0, sizeof st);
    k93css_apply(ctx->p->css, n, &st);
    if (st.flags & K93CSS_FG) sf_fg = st.fg;
    if (st.flags & K93CSS_BG) sf_bg = st.bg;
    if (st.flags & K93CSS_BOLD) sf_bold = st.bold;
    if (st.flags & K93CSS_CENTER) sf_center = st.center;
    if (st.flags & K93CSS_SCALE) sf_scale = st.scale;
    if (st.flags & K93CSS_HIDDEN) sf_hidden = 1;
    if (st.flags & K93CSS_INDENT) sf_indent += st.indent;
    if (st.flags & K93CSS_FONT) sf_font = st.font;
    if (st.flags & K93CSS_UNDERLINE) sf_underline = st.underline;
    if (st.flags & K93CSS_STRIKE) sf_strike = st.strike;
    if (st.flags & K93CSS_XFORM) sf_xform = st.xform;
    if (st.flags & K93CSS_LH) sf_lh = st.lh;
    if (st.flags & K93CSS_WIDTH) sf_width = st.width;
    if (st.flags & K93CSS_LSTYLE) sf_lstyle = st.lstyle;
    if (st.flags & K93CSS_OPACITY) sf_op = sf_op * st.opacity / 100;
    if (sf_hidden) return;

    if (!strcmp(t, "br")) { flush(ctx, 1); return; }
    if (!strcmp(t, "hr")) {
        flush(ctx, 1);
        uint32_t og = ctx->fg;
        ctx->fg = PAGE_SUB;
        add_str(ctx, "----------------------------------------");
        flush(ctx, 0);
        ctx->fg = og;
        return;
    }
    if (!strcmp(t, "img")) { render_img(ctx, n); return; }
    if (!strcmp(t, "svg")) { render_svg_inline(ctx, n); return; }
    if (!strcmp(t, "iframe") || !strcmp(t, "canvas") || !strcmp(t, "object") ||
        !strcmp(t, "embed") || !strcmp(t, "video") || !strcmp(t, "audio")) {
        /* visible placeholders: media/frames/canvas live outside the engine */
        const char *src = k93html_attr(n, "src");
        flush(ctx, 0);
        uint32_t og = ctx->fg;
        char ph[192];
        snprintf(ph, sizeof ph, "[%s%s%s]", t,
                 (src && *src) ? " " : "", (src && *src) ? src : "");
        ctx->fg = PAGE_SUB;
        add_str(ctx, ph);
        ctx->fg = og;
        flush(ctx, 0);
        return;
    }
    if (!strcmp(t, "input")) { render_widget(ctx, n); return; }
    if (!strcmp(t, "select")) { render_select(ctx, n); return; }

    uint32_t og_fg = ctx->fg, og_bg = ctx->bg;
    int og_bold = ctx->bold, og_center = ctx->center, og_scale = ctx->scale;
    int og_hidden = ctx->hidden, og_indent = ctx->indent, og_pre = ctx->in_pre;
    int og_font = ctx->font, og_underline = ctx->underline;
    int og_strike = ctx->strike, og_xform = ctx->xform;
    int og_op = ctx->op, og_lh = ctx->lh, og_width = ctx->width;
    int og_lstyle = ctx->lstyle, og_ltype = ctx->ltype;
    uint32_t og_segbg = ctx->segbg, og_bordc = ctx->bordc;
    int og_olnum = ctx->ol_num, og_bord = ctx->bord;
    ctx->fg = fade_to_white(sf_fg, sf_op);
    ctx->bg = sf_bg ? fade_to_white(sf_bg, sf_op) : 0;
    ctx->bold = sf_bold;
    ctx->center = sf_center;
    ctx->scale = sf_scale;
    ctx->hidden = sf_hidden;
    ctx->indent = sf_indent;
    ctx->font = sf_font;
    ctx->underline = sf_underline;
    ctx->strike = sf_strike;
    ctx->xform = sf_xform;
    ctx->op = sf_op;
    ctx->lh = sf_lh;
    ctx->width = sf_width;
    ctx->lstyle = sf_lstyle;
    ctx->segbg = sf_segbg;

    int block = is_block(t);
    int heading = (t[0] == 'h' && t[1] >= '1' && t[1] <= '6' && t[2] == 0);
    int bordered = 0;                      /* px of border box indent */

    if (block) flush(ctx, 0);
    if (heading) {
        ctx->scale = t[1] <= '2' ? 2 : 1;
        ctx->bold = 1;
        flush(ctx, 1);                     /* space above headings */
    }
    if (block && (st.flags & K93CSS_BORDER) && st.bordw > 0 && !ctx->bord) {
        uint32_t bc = st.bordc ? st.bordc : 0x9AA3AEu;
        ctx->bordc = bc;
        emit_rule(ctx, '-', bc);
        ctx->bord = st.bordw;
        bordered = st.bordw + 6;
        ctx->indent += bordered;
    } else if (!strcmp(t, "fieldset") && !ctx->bord) {
        ctx->bordc = 0x9AA3AEu;
        emit_rule(ctx, '-', ctx->bordc);
        ctx->bord = 1;
        bordered = 7;
        ctx->indent += bordered;
    }
    if (!strcmp(t, "pre")) ctx->in_pre = 1;
    if (!strcmp(t, "b") || !strcmp(t, "strong") || !strcmp(t, "th"))
        ctx->bold = 1;
    if (!strcmp(t, "summary")) ctx->bold = 1;
    if (!strcmp(t, "code") || !strcmp(t, "tt") || !strcmp(t, "kbd") ||
        !strcmp(t, "samp"))
        ctx->fg = 0x8A2B2Bu;
    if (!strcmp(t, "kbd")) ctx->bold = 1;
    if (!strcmp(t, "var")) ctx->fg = 0x663399u;
    if (!strcmp(t, "cite") || !strcmp(t, "dfn") || !strcmp(t, "address"))
        ctx->fg = PAGE_SUB;
    if (!strcmp(t, "u") || !strcmp(t, "ins")) ctx->underline = 1;
    if (!strcmp(t, "del") || !strcmp(t, "s") || !strcmp(t, "strike"))
        ctx->strike = 1;
    if (!strcmp(t, "mark")) ctx->segbg = 0xFFF3A6;
    if (!strcmp(t, "blockquote")) ctx->indent += 24;
    if (!strcmp(t, "progress") || !strcmp(t, "meter")) {
        flush(ctx, 0);
        double v = 0, mx = 100;
        const char *va = k93html_attr(n, "value");
        if (va && *va) v = atof(va);
        else {
            char tmp[32];
            k93html_text_content(n, tmp, sizeof tmp);
            v = atof(tmp);
        }
        const char *ma = k93html_attr(n, "max");
        if (ma && *ma) mx = atof(ma);
        if (mx <= 0) mx = 100;
        render_bar(ctx, (int)(v * 100.0 / mx + 0.5));
        flush(ctx, 0);
        goto out;
    }
    if (!strcmp(t, "textarea")) {
        flush(ctx, 0);
        int idx = link_register(ctx->p, "", n);
        if (idx >= 0) { ctx->in_anchor = 1; ctx->anchor_link = idx; }
        char txt[256];
        const char *va = k93html_attr(n, "value");
        if (va && *va) k93_strlcpy(txt, va, sizeof txt);
        else k93html_text_content(n, txt, sizeof txt);
        for (char *q = txt; *q; q++)
            if (*q == '\n' || *q == '\r' || *q == '\t') *q = ' ';
        uint32_t ogf = ctx->fg, osg = ctx->segbg;
        ctx->segbg = 0xF1F4F8;
        if (!txt[0]) {
            const char *ph = k93html_attr(n, "placeholder");
            ctx->fg = PAGE_SUB;
            k93_strlcpy(txt, (ph && *ph) ? ph : " ", sizeof txt);
        }
        add_str(ctx, "[ ");
        add_str(ctx, txt);
        add_str(ctx, " ]");
        ctx->fg = ogf;
        ctx->segbg = osg;
        ctx->in_anchor = 0;
        ctx->anchor_link = -1;
        flush(ctx, 0);
        goto out;
    }
    if (!strcmp(t, "ul")) {
        ctx->indent += 20;
        ctx->ltype = 1;
        ctx->lstyle = (st.flags & K93CSS_LSTYLE) ? st.lstyle : K93L_DISC;
        render_children(ctx, n);
        flush(ctx, 0);
        goto out;
    }
    if (!strcmp(t, "ol")) {
        ctx->indent += 20;
        ctx->ltype = 2;
        ctx->lstyle = (st.flags & K93CSS_LSTYLE) ? st.lstyle : K93L_DECIMAL;
        const char *sa = k93html_attr(n, "start");
        ctx->ol_num = sa ? atoi(sa) : 1;
        render_children(ctx, n);
        ctx->ol_num = og_olnum;
        flush(ctx, 0);
        goto out;
    }
    if (!strcmp(t, "li")) {
        char mark[24];
        if (ctx->ltype == 2) {
            if (ctx->lstyle == K93L_LALPHA || ctx->lstyle == K93L_UALPHA ||
                ctx->lstyle == K93L_LROMAN || ctx->lstyle == K93L_UROMAN ||
                ctx->lstyle == K93L_DECIMAL)
                list_marker(mark, sizeof mark, ctx->lstyle, 1, ctx->ol_num);
            else
                list_marker(mark, sizeof mark, K93L_DECIMAL, 1, ctx->ol_num);
            ctx->ol_num++;
        } else {
            list_marker(mark, sizeof mark, ctx->lstyle, 0, 0);
        }
        uint32_t ogf = ctx->fg;
        ctx->fg = PAGE_SUB;
        add_str(ctx, mark);
        ctx->fg = ogf;
        render_children(ctx, n);
        flush(ctx, 0);
        goto out;
    }
    if (!strcmp(t, "dt")) {
        ctx->bold = 1;
        render_children(ctx, n);
        flush(ctx, 0);
        goto out;
    }
    if (!strcmp(t, "dd")) {
        ctx->indent += 20;
        render_children(ctx, n);
        flush(ctx, 0);
        goto out;
    }
    if (!strcmp(t, "table")) {
        render_table(ctx, n);
        goto out;
    }
    if (!strcmp(t, "tr")) {
        flush(ctx, 0);
        int cell = 0;
        for (k93node *c = n->first; c; c = c->next) {
            if (strcmp(c->tag, "td") && strcmp(c->tag, "th")) {
                render_node(ctx, c);
                continue;
            }
            if (cell++ > 0) {
                uint32_t og = ctx->fg;
                ctx->fg = PAGE_SUB;
                add_str(ctx, " | ");
                ctx->fg = og;
            }
            render_node(ctx, c);
        }
        flush(ctx, 0);
        goto out;
    }
    if (!strcmp(t, "a")) {
        const char *href = k93html_attr(n, "href");
        const char *onclick = k93html_attr(n, "onclick");
        int idx = -1;
        if ((href && href[0] && strcmp(href, "#")) || onclick) {
            char abs[1280] = "";
            if (href && href[0] && strcmp(href, "#"))
                url_absolutize(ctx->p->base, href, abs, sizeof abs);
            idx = link_register(ctx->p, abs, n);
        }
        if (idx >= 0) {
            ctx->in_anchor = 1;
            ctx->anchor_link = idx;
        }
        render_children(ctx, n);
        ctx->in_anchor = 0;
        ctx->anchor_link = -1;
        goto out;
    }
    if (!strcmp(t, "button")) {            /* clickable block */
        int idx = link_register(ctx->p, "", n);
        if (idx >= 0) { ctx->in_anchor = 1; ctx->anchor_link = idx; }
        ctx->bold = 1;
        ctx->segbg = 0xE9EEF4;
        add_str(ctx, "[ ");
        render_children(ctx, n);
        add_str(ctx, " ]");
        ctx->segbg = og_segbg;
        ctx->in_anchor = 0;
        ctx->anchor_link = -1;
        flush(ctx, 0);
        goto out;
    }

    render_children(ctx, n);
    if (block) flush(ctx, 0);
    if (heading) flush(ctx, 1);
out:
    if (bordered) {
        flush(ctx, 0);
        ctx->bord = 0;
        ctx->indent -= bordered;
        emit_rule(ctx, '-', ctx->bordc);
    }
    ctx->fg = og_fg;
    ctx->bg = og_bg;
    ctx->bold = og_bold;
    ctx->center = og_center;
    ctx->scale = og_scale;
    ctx->hidden = og_hidden;
    ctx->indent = og_indent;
    ctx->font = og_font;
    ctx->underline = og_underline;
    ctx->strike = og_strike;
    ctx->xform = og_xform;
    ctx->op = og_op;
    ctx->lh = og_lh;
    ctx->width = og_width;
    ctx->lstyle = og_lstyle;
    ctx->ltype = og_ltype;
    ctx->segbg = og_segbg;
    ctx->bord = og_bord;
    ctx->bordc = og_bordc;
    ctx->in_pre = og_pre;
}

void sf_render(sf_page *p, k93node *doc)
{
    p->pg.n = 0;
    p->nruns = 0;
    p->pused = 0;
    p->nlinks = 0;
    rctx ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.p = p;
    ctx.fg = PAGE_INK;
    ctx.font = -1;
    ctx.anchor_link = -1;
    ctx.op = 100;                       /* opacity percent */

    /* header: title + URL + spacer */
    char tbuf[256];
    sf_title(doc, tbuf, sizeof tbuf);
    ui_pline *l = line_begin(p, 2, 0, 0, 0);
    line_run(p, l, tbuf, (int)strlen(tbuf), PAGE_INK, 1, -1, -1, 0, 0);
    l = line_begin(p, 1, 0, 0, 0);
    line_run(p, l, p->base, (int)strlen(p->base), PAGE_SUB, 0, -1, -1, 0, 0);
    line_begin(p, 1, 0, 0, 0);              /* spacer */

    render_children(&ctx, sf_body(doc));
    flush(&ctx, 0);
}
