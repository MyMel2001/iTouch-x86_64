/* Safari.app - web browser (clean-room).
 * iOS 4.3.4 shipped MobileSafari (WebKit). This OS cannot reuse WebKit (ARM,
 * and not clean-room), so Safari here is a small hand-written engine:
 *   - HTTP/HTTPS via libk93 k93net (proxy.conf + HTTP(S)_PROXY, mbedTLS)
 *   - HTML tree parser + CSS subset w/ specificity (libk93 k93html), styled
 *     line renderer with inline runs, links, <img> (stb_image) and SVG
 *     (k93svg, linked + inline) in k93ui's ui_pageview
 *   - JavaScript via QuickJS (MIT, vendored): DOM bindings, timers, fetch,
 *     XHR, events, classList/dataset/style, DOMContentLoaded
 * Ceiling: no canvas/WebGL, no frames; images fetched synchronously when a
 * page renders. */
#define _GNU_SOURCE
#include "safari.h"
#include "k93ui.h"
#include "k93util.h"
#include "k93app.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <ctype.h>
static struct {
    char base[1280];                     /* absolute base for relative links */
} B;

/* sf_fetch: GET url (k93net follows redirects, capped) */
static size_t sf_last_len;
char *sf_fetch(const char *url, int *status, char *err, size_t errlen)
{
    return k93net_get(url, &sf_last_len, status, err, errlen);
}

size_t sf_fetch_len(void) { return sf_last_len; }

/* ---------------------------------------------------------------- render */

static int pview_tick(ui_page *pgv, void *vpg)
{
    (void)pgv;
    sf_page *p = vpg;
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

static int browse_d(const char *url_in, int depth)
{
    struct url u;
    char url[1280];
    k93_strlcpy(url, url_in, sizeof url);
    k93_trim(url);
    if (url_parse(url, &u) < 0) {
        ui_msgbox("Safari", "Cannot parse URL");
        return -1;
    }
    B.base[0] = 0;
    snprintf(B.base, sizeof B.base, "%s://%s:%d%s", u.scheme, u.host, u.port, u.path);

    ui_statusmsg("Contacting host ...");
    int status = 0;
    char err[128] = "";
    struct k93conn *kc = k93net_open(&u, err, sizeof err);
    if (!kc) {
        char m[256];
        snprintf(m, sizeof m,
                 "Cannot connect to %s:%d\n\n%s\n\n(check network and proxy in Settings)",
                 u.host, u.port, err[0] ? err : "cannot connect");
        ui_msgbox("Safari", m);
        return -1;
    }
    char *raw = k93net_read_http(kc, &u, &status, err, sizeof err);
    k93net_close(kc);
    if (!raw) {
        char m[192];
        snprintf(m, sizeof m, "Fetch failed: %s", err[0] ? err : "unknown error");
        ui_msgbox("Safari", m);
        return -1;
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
        return browse_d(abs, depth + 1);
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
        int rv = ui_pageview(title[0] ? title : "Safari", &pg.pg, pview_tick, &pg);
        if (rv < 0 || rv >= pg.nlinks) break;
        sf_link *L = &pg.links[rv];
        int handled = js_click(L);
        if (js_tick()) continue;         /* DOM changed: re-render, stay */
        if (js_nav_url()) break;         /* location changed: navigate */
        if (handled != 2 && L->url[0]) {
            if (!strncmp(L->url, "javascript:", 11)) continue;
            k93_strlcpy(navurl, L->url, sizeof navurl);
            nav = 1;
            break;
        }
    }
    const char *nu = js_nav_url();
    if (nu && !navurl[0]) {
        k93_strlcpy(navurl, nu, sizeof navurl);
        nav = 1;
    }
    js_nav_clear();
    js_close();
    sf_page_free(&pg);
    k93css_free(css);
    k93html_free(doc);
    if (nav && depth < 16) return browse_d(navurl, depth + 1);
    return 0;
}

static int browse(const char *url_in) { return browse_d(url_in, 0); }

static int url_ok(const char *s) { struct url t; return url_parse(s, &t) == 0; }

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
        if (ui_input("Address (URL)", input, sizeof input) < 0) break;
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
