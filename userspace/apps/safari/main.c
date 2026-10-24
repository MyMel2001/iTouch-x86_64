/* Safari.app - web browser (clean-room).
 * iOS 4.3.4 shipped MobileSafari (WebKit). This OS cannot reuse WebKit (ARM,
 * and not clean-room), so Safari here is a small hand-written engine:
 *   - HTTP/HTTPS via libk93 k93net (proxy.conf + HTTP(S)_PROXY, mbedTLS)
 *   - HTML tree parser + CSS subset w/ specificity (libk93 k93html), styled
 *     line renderer with inline runs, links, <img> (stb_image) and SVG
 *     (k93svg, linked + inline) in k93ui's ui_pageview
 *   - JavaScript via QuickJS (MIT, vendored): DOM bindings, timers, fetch,
 *     XHR, events, classList/dataset/style, DOMContentLoaded,
 *     canvas 2D subset (fillRect/clearRect/strokeRect/fillText/paths)
 *   - media/frames: <iframe> documents render inline (scripts inside frames
 *     do not run), <video>/<audio> show poster + metadata (no codecs in the
 *     OS, playback unsupported), <object>/<embed> decode images or show
 *     metadata + fallback content, <details>/<summary> toggle on click
 *   - browser chrome: history back/forward (Left/Right), reload (r),
 *     bookmarks add/open (b, stored in /var/mobile/Library/Safari),
 *     find-in-page (f, case-insensitive, wraps)
 * Ceiling: no WebGL/2D-extras (no transforms/fonts API), no media playback,
 * no plugins (PDF), frames run no scripts; images fetched synchronously
 * when a page renders. */
#define _GNU_SOURCE
#include "safari.h"
#include "k93ui.h"
#include "k93util.h"
#include "k93app.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <ctype.h>
static struct {
    char base[1280];                     /* absolute base for relative links */
} B;

/* ---- browser chrome state: history stack + bookmarks ---- */
#define SF_HIST 96
#define SF_BM_DIR  "/var/mobile/Library/Safari"
#define SF_BM_FILE SF_BM_DIR "/bookmarks.txt"

static char hist[SF_HIST][1280];
static int  hist_n, hist_cur = -1;
static char sf_hist_nav[1280];           /* pending chrome navigation */
static int  sf_hist_from;                /* 1 = history back/forward (no push) */
static char cur_url[1280];               /* URL of the page on view */

static void sf_hist_push(const char *url)
{
    if (hist_cur >= 0 && !strcmp(hist[hist_cur], url)) return;
    if (hist_n >= SF_HIST) {             /* drop oldest */
        memmove(hist[0], hist[1], sizeof(hist[0]) * (size_t)(SF_HIST - 1));
        hist_n--; hist_cur--;
    }
    if (hist_cur + 1 < hist_n) hist_n = hist_cur + 1;   /* truncate forward */
    hist_cur++;
    k93_strlcpy(hist[hist_cur], url, sizeof hist[0]);
    hist_n = hist_cur + 1;
}

/* bookmarks: plain text, one "url<TAB>title" per line */
static int sf_bm_load(char urls[][1280], char titles[][128], int max)
{
    size_t len = 0;
    char *t = k93_read_file(SF_BM_FILE, &len);
    int n = 0;
    if (!t) return 0;
    char *p = t, *end = t + len;
    while (p < end && n < max) {
        char *nl = memchr(p, '\n', (size_t)(end - p));
        char *line = nl ? nl : end;
        char *tab = memchr(p, '\t', (size_t)(line - p));
        size_t ul = tab ? (size_t)(tab - p) : (size_t)(line - p);
        size_t tl = tab ? (size_t)(line - tab - 1) : 0;
        if (ul >= sizeof urls[0]) ul = sizeof urls[0] - 1;
        memcpy(urls[n], p, ul); urls[n][ul] = 0;
        if (tl >= sizeof titles[0]) tl = sizeof titles[0] - 1;
        memcpy(titles[n], tab ? tab + 1 : line, tl); titles[n][tl] = 0;
        if (urls[n][0]) n++;
        if (!nl) break;
        p = nl + 1;
    }
    free(t);
    return n;
}

static void sf_bm_add(const char *url, const char *title)
{
    k93_mkdir_p(SF_BM_DIR, 0777);
    char line[1500];
    snprintf(line, sizeof line, "%s\t%.100s\n", url, title ? title : "");
    k93_write_file(SF_BM_FILE, line, strlen(line), 1);   /* append */
}

/* find-in-page: move selection to next line containing term (case-insens),
 * wrapping around. Returns line index or -1. */
static int sf_find(sf_page *p, const char *term)
{
    int tl = (int)strlen(term);
    if (tl < 1) return -1;
    for (int pass = 0; pass < 2; pass++) {
        int start = pass ? 0 : p->pg.sel + 1;
        int stop  = pass ? p->pg.sel + 1 : p->pg.n;
        for (int i = start; i < stop; i++) {
            for (int r = 0; r < p->pg.lines[i].nruns; r++) {
                const char *s = p->pg.lines[i].runs[r].text;
                for (; *s; s++)
                    if (!strncasecmp(s, term, (size_t)tl)) { p->pg.sel = i; return i; }
            }
        }
    }
    return -1;
}

/* chrome keys: Left/Right history, r reload, f find, b bookmarks */
static int chrome_key(int k, ui_page *pg, void *ctx)
{
    sf_page *p = ctx;
    (void)pg;
    if (k == UI_KEY_LEFT) {
        if (hist_cur > 0) {
            hist_cur--;
            k93_strlcpy(sf_hist_nav, hist[hist_cur], sizeof sf_hist_nav);
            sf_hist_from = 1;
            return 1;
        }
        return 0;
    }
    if (k == UI_KEY_RIGHT) {
        if (hist_cur < hist_n - 1) {
            hist_cur++;
            k93_strlcpy(sf_hist_nav, hist[hist_cur], sizeof sf_hist_nav);
            sf_hist_from = 1;
            return 1;
        }
        return 0;
    }
    if (k == UI_KEY_CHAR_BASE + 'r') {
        k93_strlcpy(sf_hist_nav, cur_url, sizeof sf_hist_nav);
        sf_hist_from = 0;
        return 1;
    }
    if (k == UI_KEY_CHAR_BASE + 'f') {
        char term[128];
        term[0] = 0;
        if (ui_input("Find on page", term, sizeof term) == 0 && term[0]) {
            if (sf_find(p, term) < 0) ui_statusmsg("Not found");
            else ui_statusmsg("Found");
        }
        return 1;
    }
    if (k == UI_KEY_CHAR_BASE + 'b') {
        static char urls[64][1280];
        static char titles[64][128];
        const char *items[66];
        int n = sf_bm_load(urls, titles, 64);
        items[0] = "Add current page to bookmarks";
        for (int i = 0; i < n; i++) {
            char *it = k93_xmalloc(200);
            snprintf(it, 200, "%.60s  %.60s", titles[i], urls[i]);
            items[i + 1] = it;
        }
        int sel = ui_menu("Bookmarks", items, n + 1, 0);
        if (sel == 0) {
            char t[96];
            sf_title(p->doc, t, sizeof t);
            sf_bm_add(cur_url, t);
            ui_statusmsg("Bookmark added");
        } else if (sel > 0 && sel <= n) {
            k93_strlcpy(sf_hist_nav, urls[sel - 1], sizeof sf_hist_nav);
            sf_hist_from = 0;
        }
        for (int i = 0; i < n; i++) free((void *)items[i + 1]);
        return 1;
    }
    return 0;
}

/* sf_fetch: GET url (k93net follows redirects, capped); file:// reads
 * local files (all real browsers support this) */
static size_t sf_last_len;
char *sf_fetch(const char *url, int *status, char *err, size_t errlen)
{
    if (!strncmp(url, "file://", 7)) {
        FILE *f = fopen(url + 7, "rb");
        if (!f) {
            *status = 0;
            sf_last_len = 0;
            snprintf(err, errlen, "cannot open %.40s", url + 7);
            return NULL;
        }
        size_t cap = 65536, n = 0;
        char *buf = malloc(cap);
        while (buf) {
            n += fread(buf + n, 1, cap - n - 1, f);
            if (n + 1 < cap) break;
            char *nb = realloc(buf, cap * 2);
            if (!nb) { free(buf); buf = NULL; break; }
            buf = nb;
            cap *= 2;
        }
        fclose(f);
        if (!buf) {
            *status = 0;
            sf_last_len = 0;
            snprintf(err, errlen, "out of memory");
            return NULL;
        }
        buf[n] = 0;
        sf_last_len = n;
        *status = 200;
        err[0] = 0;
        return buf;
    }
    return k93net_get(url, &sf_last_len, status, err, errlen);
}

size_t sf_fetch_len(void) { return sf_last_len; }

/* ---------------------------------------------------------------- render */

static int pview_tick(ui_page *pgv, void *vpg)
{
    (void)pgv;
    sf_page *p = vpg;
    if (sf_hist_nav[0]) return 2;        /* chrome history/bookmark/reload */
    if (js_nav_url()) return 2;          /* close view; browse loop navigates */
    if (js_tick()) {                     /* DOM changed: rebuild + redraw */
        sf_render(p, p->doc);
        return 1;
    }
    return 0;
}

/* non-HTML payload -> <pre> document */
static k93node *doc_from_text(const char *s)
{
    size_t cap = strlen(s) * 6 + 32;
    char *esc = k93_xmalloc(cap);
    size_t o = 0;
    o += (size_t)sprintf(esc + o, "<pre>");
    for (; *s && o + 8 < cap; s++) {
        if (*s == '&') o += (size_t)sprintf(esc + o, "&amp;");
        else if (*s == '<') o += (size_t)sprintf(esc + o, "&lt;");
        else if (*s == '>') o += (size_t)sprintf(esc + o, "&gt;");
        else esc[o++] = *s;
    }
    (void)sprintf(esc + o, "</pre>");
    k93node *d = k93html_parse(esc);
    free(esc);
    return d;
}

static void css_collect(k93css *css, k93node *doc)
{
    k93node *sty[32];
    int ns = k93html_query_all(doc, "style", sty, 32);
    for (int i = 0; i < ns; i++)
        if (sty[i]->text) k93css_add(css, sty[i]->text);
    /* linked stylesheets (small; failures ignored) */
    k93node *lnks[32];
    int nl = k93html_query_all(doc, "link", lnks, 32);
    for (int i = 0; i < nl; i++) {
        const char *rel = k93html_attr(lnks[i], "rel");
        const char *href = k93html_attr(lnks[i], "href");
        if (!rel || !strcasestr(rel, "stylesheet") || !href) continue;
        char abs[1280];
        int status = 0;
        char err[64];
        url_absolutize(B.base, href, abs, sizeof abs);
        char *css_txt = sf_fetch(abs, &status, err, sizeof err);
        if (css_txt && status >= 200 && status < 300) {
            k93css_add(css, css_txt);
        }
        free(css_txt);
    }
}

/* ---------------------------------------------------------------- app */

static int browse(const char *url_in);

static int browse_d(const char *url_in, int depth, int from_hist)
{
    struct url u;
    char url[1280];
    k93_strlcpy(url, url_in, sizeof url);
    k93_trim(url);
    k93_strlcpy(cur_url, url, sizeof cur_url);
    sf_hist_nav[0] = 0;
    if (!from_hist) sf_hist_push(url);
    int status = 0;
    char err[128] = "";
    char *raw = NULL;
    int local = !strncmp(url, "file://", 7);
    if (local) {                         /* local file: no network involved */
        memset(&u, 0, sizeof u);
        k93_strlcpy(B.base, url, sizeof B.base);
        raw = sf_fetch(url, &status, err, sizeof err);
        if (!raw) {
            char m[256];
            snprintf(m, sizeof m, "Cannot open file:\n\n%.80s\n\n%s",
                     url + 7, err[0] ? err : "unreadable");
            ui_msgbox("Safari", m);
            return -1;
        }
    } else {
        if (url_parse(url, &u) < 0) {
            ui_msgbox("Safari", "Cannot parse URL");
            return -1;
        }
        B.base[0] = 0;
        snprintf(B.base, sizeof B.base, "%s://%s:%d%s",
                 u.scheme, u.host, u.port, u.path);

        ui_statusmsg("Contacting host ...");
        struct k93conn *kc = k93net_open(&u, err, sizeof err);
        if (!kc) {
            char m[256];
            snprintf(m, sizeof m,
                     "Cannot connect to %s:%d\n\n%s\n\n(check network and proxy in Settings)",
                     u.host, u.port, err[0] ? err : "cannot connect");
            ui_msgbox("Safari", m);
            return -1;
        }
        raw = k93net_read_http(kc, &u, &status, err, sizeof err);
        k93net_close(kc);
        if (!raw) {
            char m[192];
            snprintf(m, sizeof m, "Fetch failed: %s", err[0] ? err : "unknown error");
            ui_msgbox("Safari", m);
            return -1;
        }
    }
    if (status >= 300 && status < 400 && k93net_last_location()[0]) {
        if (depth >= 8) {
            free(raw);
            ui_msgbox("Safari", "Too many redirects");
            return -1;
        }
        char abs[1280];
        url_absolutize(url, k93net_last_location(), abs, sizeof abs);
        free(raw);
        return browse_d(abs, depth + 1, 0);
    }
    if (status >= 400) {
        char m[128];
        snprintf(m, sizeof m, "Server returned HTTP %d", status);
        ui_msgbox("Safari", m);
    }

    /* package download: .ipa/.deb links save to Downloads and offer install
     * (same clean-room path as Files.app; no signature checks exist here) */
    {
        size_t pl = strlen(u.path);
        const char *base = strrchr(u.path, '/');
        base = base ? base + 1 : u.path;
        int ispkg = (pl > 4 && (!strcasecmp(u.path + pl - 4, ".ipa") ||
                                !strcasecmp(u.path + pl - 4, ".deb")));
        if (ispkg && base[0]) {
            char dest[1200], q[1400];
            k93_mkdir_p(K93_DOWNLOADS_DIR, 0755);
            snprintf(dest, sizeof dest, "%s/%s", K93_DOWNLOADS_DIR, base);
            snprintf(q, sizeof q,
                     "Download package?\n\n%s\n\nSave to %s and install?",
                     u.path, K93_DOWNLOADS_DIR);
            if (ui_confirm("Safari", q)) {
                if (k93_write_file(dest, raw, k93net_last_len(), 0) < 0) {
                    ui_msgbox("Safari", "Cannot write download (disk full?)");
                } else {
                    ui_statusmsg("Installing package ...");
                    char err2[256] = "";
                    int rc = (pl > 4 && !strcasecmp(u.path + pl - 4, ".ipa"))
                             ? k93_app_install_ipa(dest, err2, sizeof err2)
                             : k93_app_install_deb(dest, err2, sizeof err2);
                    if (rc == 0) {
                        char m[1400];
                        snprintf(m, sizeof m, "Installed %s\n(copy kept in Downloads)", base);
                        ui_msgbox("Safari", m);
                    } else {
                        ui_msgbox("Install failed", err2);
                    }
                }
            }
            free(raw);
            return 0;
        }
    }

    /* build DOM (HTML or plain text) */
    const char *b = raw;
    while (*b == ' ' || *b == '\n' || *b == '\r' || *b == '\t') b++;
    k93node *doc = (*b && *b != '<') ? doc_from_text(raw) : k93html_parse(raw);
    free(raw);
    if (!doc) {
        ui_msgbox("Safari", "Out of memory");
        return -1;
    }

    k93css *css = k93css_new();
    css_collect(css, doc);

    sf_page pg;
    sf_page_init(&pg, ui_px_width() - 48 - 28);
    k93_strlcpy(pg.base, B.base, sizeof pg.base);
    pg.css = css;
    pg.doc = doc;

    char title[96];
    js_open(&pg, doc, B.base);
    js_run_scripts(doc);

    int nav = 0;
    char navurl[1280];
    navurl[0] = 0;
    for (;;) {
        sf_title(doc, title, sizeof title);
        sf_render(&pg, doc);
        int rv = ui_pageview(title[0] ? title : "Safari", &pg.pg,
                             pview_tick, chrome_key, &pg);
        if (rv >= 0 && rv < pg.nlinks) {
            sf_link *L = &pg.links[rv];
            if (L->node && !strcmp(L->node->tag, "summary")) {   /* details toggle */
                k93node *d = L->node->parent;
                if (d && !strcmp(d->tag, "details")) {
                    sf_details_toggle(d);
                    continue;              /* rebuild + redraw */
                }
            }
            int handled = js_click(L);
            if (js_nav_url()) { sf_hist_from = 0; break; } /* location changed */
            if (handled != 2 && L->url[0]) { /* (nav wins over dirty re-render) */
                if (L->url[0] == '#') continue; /* fragment: no scroll-to-anchor */
                if (!strncmp(L->url, "javascript:", 11)) {
                    js_run_url(L->url);
                    if (js_nav_url()) { sf_hist_from = 0; break; }
                    continue;
                }
                const char *hs = strchr(L->url, '#');
                if (hs && hs == L->url + strlen(B.base) &&
                    !strncmp(L->url, B.base, strlen(B.base)))
                    continue;               /* same-page fragment: stay */
                if (strncmp(L->url, "http://", 7) && strncmp(L->url, "https://", 8))
                    continue;               /* mailto:/tel:/data: no handler */
                k93_strlcpy(navurl, L->url, sizeof navurl);
                sf_hist_from = 0;
                nav = 1;
                break;
            }
            continue;
        }
        if (rv < 0 && !sf_hist_nav[0]) break;  /* chrome nav: fall through */
    }
    const char *nu = js_nav_url();
    if (nu && !navurl[0]) {
        k93_strlcpy(navurl, nu, sizeof navurl);
        sf_hist_from = 0;
        nav = 1;
    } else if (sf_hist_nav[0] && !navurl[0]) {
        k93_strlcpy(navurl, sf_hist_nav, sizeof navurl);
        nav = 1;
    }
    js_nav_clear();
    js_close();
    sf_page_free(&pg);
    k93css_free(css);
    k93html_free(doc);
    if (nav && depth < 16) return browse_d(navurl, depth + 1, sf_hist_from);
    return 0;
}

static int browse(const char *url_in) { return browse_d(url_in, 0, 0); }

static int url_ok(const char *s)
{
    if (!strncmp(s, "file://", 7)) return 1;
    struct url t;
    return url_parse(s, &t) == 0;
}

int main(int argc, char **argv)
{
    if (ui_init("Safari") < 0) return 1;
    k93net_init();
    k93net_set_ua("K93Safari/2.0 (clean-room)");
    char url[1280] = "http://info.cern.ch/";
    if (argc > 1) k93_strlcpy(url, argv[1], sizeof url);

    /* honor HTTP(S)_PROXY env as well (standard Linux logic, per NOTE 3) */
    const char *ep = getenv("HTTPS_PROXY");
    if (!ep) ep = getenv("HTTP_PROXY");
    if (ep) {
        struct url pu;
        char tmp[1280];
        if (k93_starts_with(ep, "http://")) ep += 7;
        snprintf(tmp, sizeof tmp, "http://%s", ep);
        if (url_parse(tmp, &pu) == 0) k93net_set_proxy(pu.host, pu.port);
    }

    char cur[1280];
    k93_strlcpy(cur, url, sizeof cur);
    if (argc > 1) {                     /* handed a URL (e.g. Maps handoff) */
        ui_statusmsg("Loading ...");
        browse(cur);
    }
    for (;;) {
        char input[1280];
        k93_strlcpy(input, cur, sizeof input);
        if (ui_input("Address (URL)", input, sizeof input) != 0) break; /* ESC/cancel returns 1 */
        k93_trim(input);
        if (!input[0]) continue;
        if (url_ok(input)) {
            k93_strlcpy(cur, input, sizeof cur);
        } else {
            char abs[1408];
            snprintf(abs, sizeof abs, "http://%s", input);
            if (!url_ok(abs)) {
                ui_msgbox("Safari", "Cannot parse URL");
                continue;
            }
            k93_strlcpy(cur, abs, sizeof cur);
        }
        ui_statusmsg("Loading ...");
        browse(cur);
    }
    ui_shutdown();
    return 0;
}
