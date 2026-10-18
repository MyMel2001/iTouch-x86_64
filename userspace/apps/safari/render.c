/* render.c - DOM -> ui_page lines. CSS cascade (k93html), inline runs with
 * per-run color/bold/link, word wrap, <img> fetch+decode via stb_image with
 * a small per-page cache. ASCII-only glyphs (8x16 font ceiling). */
#define _GNU_SOURCE
#include "safari.h"
#include "k93util.h"
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

struct seg { int off, len; uint32_t fg; int bold; int link; };

typedef struct {
    sf_page *p;
    uint32_t fg, bg;
    int bold, center, scale, hidden, in_pre, in_anchor;
    int indent;                        /* px */
    int anchor_link;
    int ol_num;                        /* ordered-list counter */
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
    int o = 0;
    for (int i = 0; i < len; i++) {          /* UTF-8 multibyte -> '?' */
        unsigned char c = (unsigned char)s[i];
        if (c < 0x80) { d[o++] = (char)c; continue; }
        int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
        d[o++] = '?';
        i += extra;
        len -= extra;
    }
    d[o] = 0;
    p->pused += (size_t)o + 1;
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
                     uint32_t fg, int bold, int link)
{
    if (!l || p->nruns >= p->rcap) return;
    char *t = pool_put(p, s, len);
    if (!t) return;
    ui_prun *r = &p->rbuf[p->nruns++];
    r->text = t;
    r->fg = fg;
    r->bold = bold;
    r->link = link;
    l->nruns++;
    if (link >= 0 && l->link < 0) l->link = link;
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
    if (avail < 64) avail = 64;
    struct { int off, len; uint32_t fg; int bold; int link; } lw[256];
    int nlw = 0, lwpx = 0;
    ui_pline *l = NULL;
    for (int i = 0; i < ctx->nsegs; i++) {
        struct seg *s = &ctx->segs[i];
        int off = s->off, left = s->len;
        while (left > 0) {
            const char *w = ctx->tbuf + off;
            int room = avail - lwpx;
            int ww = ui_text_width(scale, w);
            if (ww <= room) {
                if (nlw < 256) {
                    lw[nlw].off = off; lw[nlw].len = left;
                    lw[nlw].fg = s->fg; lw[nlw].bold = s->bold;
                    lw[nlw].link = s->link;
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
                lw[nlw].fg = s->fg; lw[nlw].bold = s->bold;
                lw[nlw].link = s->link;
                nlw++;
            }
            off += cut;
            left -= cut;
            if (left > 0 && w[cut] == ' ') { off++; left--; }
            /* emit the full line */
            l = line_begin(p, scale, ctx->center, ctx->indent, ctx->bg);
            for (int k = 0; k < nlw && l; k++)
                line_run(p, l, ctx->tbuf + lw[k].off, lw[k].len,
                         lw[k].fg, lw[k].bold, lw[k].link);
            nlw = 0;
            lwpx = 0;
        }
    }
    if (nlw > 0) {
        l = line_begin(p, scale, ctx->center, ctx->indent, ctx->bg);
        for (int k = 0; k < nlw && l; k++)
            line_run(p, l, ctx->tbuf + lw[k].off, lw[k].len,
                     lw[k].fg, lw[k].bold, lw[k].link);
    }
    ctx->tlen = 0;
    ctx->nsegs = 0;
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
            ls->link == (ctx->in_anchor ? ctx->anchor_link : -1)) {
            memcpy(ctx->tbuf + ctx->tlen, s, (size_t)len);
            ctx->tlen += len;
            ls->len += len;
            return;
        }
    }
    struct seg *s2 = &ctx->segs[ctx->nsegs++];
    s2->off = ctx->tlen;
    s2->len = len;
    s2->fg = ctx->fg;
    s2->bold = ctx->bold;
    s2->link = ctx->in_anchor ? ctx->anchor_link : -1;
    memcpy(ctx->tbuf + ctx->tlen, s, (size_t)len);
    ctx->tlen += len;
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
}

static int is_skip(const char *t)
{
    static const char *s[] = { "script", "style", "head", "meta", "link",
        "title", "noscript", "template", "base", "iframe", "canvas", "svg",
        "object", "embed", "audio", "video", "option", "select",
        "input", "textarea", NULL };
    for (int i = 0; s[i]; i++) if (!strcmp(t, s[i])) return 1;
    return 0;
}

static int is_block(const char *t)
{
    static const char *b[] = { "p", "div", "section", "article", "header",
        "footer", "main", "aside", "figure", "figcaption", "blockquote",
        "address", "dt", "dd", "details", "summary", "h1", "h2", "h3", "h4",
        "h5", "h6", "ul", "ol", "table", "pre", "button", NULL };
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
            unsigned char *rgba = stbi_load_from_memory(
                (const unsigned char *)body, (int)sf_fetch_len(),
                &w, &h, &comp, 4);
            k93_log("safari", "img: url=%s status=%d len=%zu w=%d h=%d %s",
                    abs, status, sf_fetch_len(), w, h,
                    rgba ? "decoded" : stbi_failure_reason());
            free(body);
            body = NULL;
            if (rgba && w > 0 && h > 0) {
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
                if (px) {
                    int ii = ctx->p->nimg++;
                    k93_strlcpy(ctx->p->img[ii].url, abs,
                                sizeof ctx->p->img[ii].url);
                    ctx->p->img[ii].px = px;
                    ctx->p->img[ii].w = nw;
                    ctx->p->img[ii].h = nh;
                    ui_pline *l = line_begin(ctx->p, 1, 0, ctx->indent, 0);
                    if (l) {
                        l->img = px;
                        l->imgw = nw;
                        l->imgh = nh;
                    }
                    return;
                }
            }
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

    uint32_t og_fg = ctx->fg, og_bg = ctx->bg;
    int og_bold = ctx->bold, og_center = ctx->center, og_scale = ctx->scale;
    int og_hidden = ctx->hidden, og_indent = ctx->indent, og_pre = ctx->in_pre;
    ctx->fg = sf_fg;
    ctx->bg = sf_bg;
    ctx->bold = sf_bold;
    ctx->center = sf_center;
    ctx->scale = sf_scale;
    ctx->hidden = sf_hidden;
    ctx->indent = sf_indent;

    int block = is_block(t);
    int heading = (t[0] == 'h' && t[1] >= '1' && t[1] <= '6' && t[2] == 0);

    if (block) flush(ctx, 0);
    if (heading) {
        ctx->scale = t[1] <= '2' ? 2 : 1;
        ctx->bold = 1;
    }
    if (!strcmp(t, "pre")) ctx->in_pre = 1;
    if (!strcmp(t, "b") || !strcmp(t, "strong") || !strcmp(t, "th"))
        ctx->bold = 1;
    if (!strcmp(t, "code") || !strcmp(t, "tt")) ctx->fg = 0x8A2B2Bu;
    if (!strcmp(t, "ul") || !strcmp(t, "ol")) ctx->indent += 20;
    if (!strcmp(t, "li")) {
        char mark[8];
        if (ctx->ol_num > 0) snprintf(mark, sizeof mark, "%d. ", ctx->ol_num++);
        else snprintf(mark, sizeof mark, "- ");
        add_str(ctx, mark);
        render_children(ctx, n);
        flush(ctx, 0);
        goto out;
    }
    if (!strcmp(t, "ol")) {
        int og_num = ctx->ol_num;
        ctx->ol_num = 1;
        render_children(ctx, n);
        ctx->ol_num = og_num;
        flush(ctx, 0);
        goto out;
    }
    if (!strcmp(t, "table")) {
        render_children(ctx, n);
        flush(ctx, 0);
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
        render_children(ctx, n);
        ctx->in_anchor = 0;
        ctx->anchor_link = -1;
        flush(ctx, 0);
        goto out;
    }

    render_children(ctx, n);
    if (block) flush(ctx, 0);
out:
    ctx->fg = og_fg;
    ctx->bg = og_bg;
    ctx->bold = og_bold;
    ctx->center = og_center;
    ctx->scale = og_scale;
    ctx->hidden = og_hidden;
    ctx->indent = og_indent;
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
    ctx.anchor_link = -1;

    /* header: title + URL + spacer */
    char tbuf[256];
    sf_title(doc, tbuf, sizeof tbuf);
    ui_pline *l = line_begin(p, 2, 0, 0, 0);
    line_run(p, l, tbuf, (int)strlen(tbuf), PAGE_INK, 1, -1);
    l = line_begin(p, 1, 0, 0, 0);
    line_run(p, l, p->base, (int)strlen(p->base), PAGE_SUB, 0, -1);
    line_begin(p, 1, 0, 0, 0);              /* spacer */

    render_children(&ctx, sf_body(doc));
    flush(&ctx, 0);
}
