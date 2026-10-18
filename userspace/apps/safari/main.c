/* Safari.app - web browser (clean-room).
 * iOS 4.3.4 shipped MobileSafari (WebKit). This OS cannot reuse WebKit (ARM,
 * and not clean-room), so Safari here is a small hand-written engine:
 *   - HTTP/1.0 + chunked decoding, HTTPS via mbedTLS with the modern CA store
 *   - honors the HTTP proxy configured in Settings (/etc/k93/proxy.conf)
 *   - HTML tree parser + CSS subset (libk93 k93html), styled line renderer
 *     with inline links and <img> (stb_image) in k93ui's ui_pageview
 *   - JavaScript via QuickJS (MIT, vendored): DOM bindings, timers, fetch,
 *     XHR, events
 * Ceiling: no canvas/WebGL, no SVG, no frames; ASCII glyphs only; images
 * fetched synchronously when a page renders. */
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
#include <netdb.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/error.h>

#define CACERT "/etc/ssl/certs/ca-certificates.crt"
#define MAXPAGE (8 * 1024 * 1024)

static size_t http_body_len;             /* body bytes of last http_get
                                            (binary bodies may contain NUL) */
static char http_location[1280];         /* Location header of last http_get */

static struct {
    char proxy_host[128];
    int proxy_port;
    int proxy_enable;
    char base[1280];                     /* absolute base for relative links */
} B;

static void proxy_load(void)
{
    B.proxy_host[0] = 0; B.proxy_port = 0; B.proxy_enable = 0;
    size_t len = 0;
    char *c = k93_read_file("/etc/k93/proxy.conf", &len);
    if (!c) return;
    for (char *line = strtok(c, "\n"); line; line = strtok(NULL, "\n")) {
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char *k = k93_trim(line), *v = k93_trim(eq + 1);
        if (!strcmp(k, "PROXY_HOST")) k93_strlcpy(B.proxy_host, v, sizeof B.proxy_host);
        else if (!strcmp(k, "PROXY_PORT")) B.proxy_port = atoi(v);
        else if (!strcmp(k, "PROXY_ENABLE")) B.proxy_enable = atoi(v);
    }
    free(c);
}

/* ------------------------------------------------------------------- urls */

int url_parse(const char *s, struct url *u)
{
    memset(u, 0, sizeof *u);
    const char *p = s;
    if (!strncmp(p, "http://", 7)) { k93_strlcpy(u->scheme, "http", sizeof u->scheme); p += 7; u->port = 80; }
    else if (!strncmp(p, "https://", 8)) { k93_strlcpy(u->scheme, "https", sizeof u->scheme); p += 8; u->port = 443; }
    else return -1;
    const char *slash = strchr(p, '/');
    const char *colon = NULL;
    if (slash) colon = memchr(p, ':', (size_t)(slash - p));
    else colon = strchr(p, ':');
    if (colon && (!slash || colon < slash)) {
        size_t hl = (size_t)(colon - p);
        if (hl >= sizeof u->host) hl = sizeof u->host - 1;
        memcpy(u->host, p, hl);
        u->host[hl] = 0;
        u->port = atoi(colon + 1);
        if (u->port <= 0 || u->port > 65535) return -1;
    } else {
        size_t hl = slash ? (size_t)(slash - p) : strlen(p);
        if (hl >= sizeof u->host) hl = sizeof u->host - 1;
        memcpy(u->host, p, hl);
        u->host[hl] = 0;
    }
    if (!u->host[0]) return -1;
    if (slash) k93_strlcpy(u->path, slash, sizeof u->path);
    else k93_strlcpy(u->path, "/", sizeof u->path);
    return 0;
}

void url_absolutize(const char *base, const char *rel, char *out, size_t cap)
{
    while (*rel == ' ') rel++;
    if (!strncmp(rel, "http://", 7) || !strncmp(rel, "https://", 8)) {
        k93_strlcpy(out, rel, cap);
        return;
    }
    if (!strncmp(rel, "javascript:", 11) || !strncmp(rel, "mailto:", 7) ||
        !strncmp(rel, "data:", 5)) {
        k93_strlcpy(out, rel, cap);
        return;
    }
    struct url b;
    if (url_parse(base, &b) < 0) { k93_strlcpy(out, rel, cap); return; }
    char org[320];                        /* origin: scheme://host[:port] */
    int defport = (!strcmp(b.scheme, "http") && b.port == 80) ||
                  (!strcmp(b.scheme, "https") && b.port == 443);
    if (defport) snprintf(org, sizeof org, "%s://%s", b.scheme, b.host);
    else snprintf(org, sizeof org, "%s://%s:%d", b.scheme, b.host, b.port);
    if (rel[0] == '/') {
        snprintf(out, cap, "%s%s", org, rel);
        return;
    }
    /* relative to current directory */
    char dir[1280];
    k93_strlcpy(dir, b.path, sizeof dir);
    char *slash = strrchr(dir, '/');
    if (slash) slash[1] = 0;
    else k93_strlcpy(dir, "/", sizeof dir);
    snprintf(out, cap, "%s%s%s", org, dir, rel);
}

/* ---------------------------------------------------------------- transport */

struct conn {
    int fd;                            /* plain socket (also used pre-tunnel) */
    mbedtls_ssl_context ssl;
    int tls;
};

static int dial(const char *host, int port)
{
    char portstr[16];
    snprintf(portstr, sizeof portstr, "%d", port);
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, portstr, &hints, &res) != 0 || !res) return -1;
    int fd = -1;
    for (struct addrinfo *a = res; a; a = a->ai_next) {
        fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (fd < 0) continue;
        struct timeval tv = { .tv_sec = 15, .tv_usec = 0 };
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
        if (connect(fd, a->ai_addr, a->ai_addrlen) == 0) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    return fd;
}

static mbedtls_entropy_context g_entropy;
static mbedtls_ctr_drbg_context g_ctr;
static mbedtls_x509_crt g_ca;
static int g_tls_ready = 0;

static void tls_global_init(void)
{
    if (g_tls_ready) return;
    mbedtls_entropy_init(&g_entropy);
    mbedtls_ctr_drbg_init(&g_ctr);
    mbedtls_x509_crt_init(&g_ca);
    const char *pers = "k93-safari";
    if (mbedtls_ctr_drbg_seed(&g_ctr, mbedtls_entropy_func, &g_entropy,
                              (const unsigned char *)pers, strlen(pers)) != 0)
        return;
    if (mbedtls_x509_crt_parse_file(&g_ca, CACERT) < 0)
        k93_log("safari", "warning: cannot load CA bundle " CACERT);
    g_tls_ready = 1;
}

static int conn_open(struct conn *c, const struct url *u)
{
    memset(c, 0, sizeof *c);
    c->fd = -1;
    int use_tls = !strcmp(u->scheme, "https");
    const char *peer = u->host;
    int peerport = u->port;

    if (B.proxy_enable && B.proxy_host[0]) {
        c->fd = dial(B.proxy_host, B.proxy_port);
        peer = B.proxy_host;
        peerport = B.proxy_port;
    } else {
        c->fd = dial(u->host, u->port);
    }
    if (c->fd < 0) return -1;

    if (use_tls) {
        if (!g_tls_ready) { close(c->fd); c->fd = -1; return -2; }
        /* tunnel through the proxy first when one is configured */
        if (B.proxy_enable && B.proxy_host[0]) {
            char req[512];
            int n = snprintf(req, sizeof req, "CONNECT %s:%d HTTP/1.0\r\n\r\n", u->host, u->port);
            if (write(c->fd, req, (size_t)n) != n) { close(c->fd); c->fd = -1; return -1; }
            char resp[512];
            ssize_t r = read(c->fd, resp, sizeof resp - 1);
            if (r <= 0 || !strstr(resp, " 200 ")) { close(c->fd); c->fd = -1; return -3; }
        }
        mbedtls_ssl_init(&c->ssl);
        static mbedtls_ssl_config conf;
        mbedtls_ssl_config_init(&conf);
        mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
                                    MBEDTLS_SSL_PRESET_DEFAULT);
        mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_REQUIRED);
        mbedtls_ssl_conf_ca_chain(&conf, &g_ca, NULL);
        mbedtls_ssl_conf_rng(&conf, mbedtls_ctr_drbg_random, &g_ctr);
        if (mbedtls_ssl_setup(&c->ssl, &conf) != 0) { close(c->fd); c->fd = -1; return -4; }
        mbedtls_ssl_set_hostname(&c->ssl, u->host);
        mbedtls_net_context nb;
        mbedtls_net_init(&nb);
        nb.fd = c->fd;
        mbedtls_ssl_set_bio(&c->ssl, &nb, mbedtls_net_send, mbedtls_net_recv, NULL);
        int rc;
        while ((rc = mbedtls_ssl_handshake(&c->ssl)) != 0) {
            if (rc != MBEDTLS_ERR_SSL_WANT_READ && rc != MBEDTLS_ERR_SSL_WANT_WRITE) {
                char ebuf[96];
                mbedtls_strerror(rc, ebuf, sizeof ebuf);
                k93_log("safari", "TLS handshake failed: %s", ebuf);
                mbedtls_ssl_free(&c->ssl);
                close(c->fd);
                c->fd = -1;
                return -5;
            }
        }
        c->tls = 1;
    }
    (void)peer; (void)peerport;
    return 0;
}

static ssize_t conn_read(struct conn *c, void *buf, size_t n)
{
    if (c->tls) {
        int r = mbedtls_ssl_read(&c->ssl, buf, n);
        if (r < 0 && r != MBEDTLS_ERR_SSL_WANT_READ && r != MBEDTLS_ERR_SSL_WANT_WRITE) return -1;
        return r;
    }
    return read(c->fd, buf, n);
}

static int conn_write(struct conn *c, const void *buf, size_t n)
{
    const char *p = buf;
    while (n) {
        ssize_t w;
        if (c->tls) {
            w = mbedtls_ssl_write(&c->ssl, p, n);
            if (w <= 0) return -1;
        } else {
            w = write(c->fd, p, n);
            if (w <= 0) return -1;
        }
        p += w; n -= (size_t)w;
    }
    return 0;
}

static void conn_close(struct conn *c)
{
    if (c->tls) { mbedtls_ssl_close_notify(&c->ssl); mbedtls_ssl_free(&c->ssl); }
    if (c->fd >= 0) { close(c->fd); c->fd = -1; }
}

/* ---------------------------------------------------------------- fetch */

static char *http_get(struct conn *c, const struct url *u, int *status, char *err, size_t errlen)
{
    char req[2048];
    int n;
    http_location[0] = 0;
    if (B.proxy_enable && B.proxy_host[0] && !strcmp(u->scheme, "http")) {
        n = snprintf(req, sizeof req,
                     "GET %s://%s:%d%s HTTP/1.0\r\n"
                     "Host: %s\r\nUser-Agent: K93Safari/2.0 (clean-room)\r\n"
                     "Accept: text/html,*/*\r\nConnection: close\r\n\r\n",
                     u->scheme, u->host, u->port, u->path, u->host);
    } else {
        n = snprintf(req, sizeof req,
                     "GET %s HTTP/1.0\r\n"
                     "Host: %s\r\nUser-Agent: K93Safari/2.0 (clean-room)\r\n"
                     "Accept: text/html,*/*\r\nConnection: close\r\n\r\n",
                     u->path, u->host);
    }
    if (conn_write(c, req, (size_t)n) < 0) { snprintf(err, errlen, "write failed"); return NULL; }

    size_t cap = 65536, len = 0;
    char *buf = k93_xmalloc(cap);
    for (;;) {
        if (len + 8192 > cap) {
            if (cap >= MAXPAGE) break;
            cap *= 2;
            char *nb = realloc(buf, cap);
            if (!nb) break;
            buf = nb;
        }
        ssize_t r = conn_read(c, buf + len, cap - len - 1);
        if (r < 0) { snprintf(err, errlen, "read error"); break; }
        if (r == 0) break;
        len += (size_t)r;
    }
    buf[len] = 0;
    if (!len) { free(buf); snprintf(err, errlen, "empty response"); return NULL; }

    *status = 0;
    if (sscanf(buf, "HTTP/%*d.%*d %d", status) != 1) *status = 0;
    char *hdr_end = strstr(buf, "\r\n\r\n");
    size_t hdr_len = hdr_end ? (size_t)(hdr_end - buf) + 4 : len;
    int chunked = 0;
    if (hdr_end) {
        char *h = buf;
        while (h < hdr_end) {
            char *e = strstr(h, "\r\n");
            if (!e) break;
            if (!strncasecmp(h, "Transfer-Encoding:", 18) && strstr(h, "chunked")) chunked = 1;
            if (!strncasecmp(h, "Location:", 9)) {
                char *s = h + 9;
                while (*s == ' ' || *s == '\t') s++;
                size_t ln = (size_t)(e - s);
                if (ln >= sizeof http_location) ln = sizeof http_location - 1;
                memcpy(http_location, s, ln);
                http_location[ln] = 0;
            }
            h = e + 2;
        }
        memmove(buf, hdr_end + 4, len - hdr_len);
        len -= hdr_len;
        buf[len] = 0;
    }
    if (chunked) {
        /* naive chunked decoder */
        char *out = k93_xmalloc(len + 1);
        size_t o = 0;
        char *p = buf;
        for (;;) {
            char *e = strstr(p, "\r\n");
            if (!e) break;
            *e = 0;
            unsigned long sz = strtoul(p, NULL, 16);
            *e = '\r';
            p = e + 2;
            if (sz == 0) break;
            if (o + sz + 1 > len) break;
            memcpy(out + o, p, sz);
            o += sz;
            p += sz + 2;
        }
        out[o] = 0;
        free(buf);
        buf = out;
        len = o;
    }
    (void)len;
    http_body_len = len;
    return buf;
}

/* sf_fetch: GET url, follow redirects (capped), body + length */
char *sf_fetch(const char *url, int *status, char *err, size_t errlen)
{
    static int depth;
    struct url u;
    *status = 0;
    if (url_parse(url, &u) < 0) {
        snprintf(err, errlen, "bad url");
        return NULL;
    }
    struct conn c;
    if (conn_open(&c, &u) < 0) {
        snprintf(err, errlen, "cannot connect");
        return NULL;
    }
    char *raw = http_get(&c, &u, status, err, errlen);
    conn_close(&c);
    if (raw && *status >= 300 && *status < 400 && http_location[0] && depth < 8) {
        char abs[1280];
        url_absolutize(url, http_location, abs, sizeof abs);
        free(raw);
        depth++;
        raw = sf_fetch(abs, status, err, errlen);
        depth--;
        return raw;
    }
    return raw;
}

size_t sf_fetch_len(void) { return http_body_len; }

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
    struct conn c;
    if (conn_open(&c, &u) < 0) {
        char m[256];
        snprintf(m, sizeof m,
                 "Cannot connect to %s:%d\n\n%s%s\n\n(check network and proxy in Settings)",
                 u.host, u.port,
                 (strcmp(u.scheme, "https") == 0) ? "TLS/CA problem or " : "",
                 B.proxy_enable ? "proxy configured" : "no proxy");
        ui_msgbox("Safari", m);
        return -1;
    }
    int status = 0;
    char err[128] = "";
    char *raw = http_get(&c, &u, &status, err, sizeof err);
    conn_close(&c);
    if (!raw) {
        char m[192];
        snprintf(m, sizeof m, "Fetch failed: %s", err[0] ? err : "unknown error");
        ui_msgbox("Safari", m);
        return -1;
    }
    if (status >= 300 && status < 400 && http_location[0]) {
        if (depth >= 8) {
            free(raw);
            ui_msgbox("Safari", "Too many redirects");
            return -1;
        }
        char abs[1280];
        url_absolutize(url, http_location, abs, sizeof abs);
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
                if (k93_write_file(dest, raw, http_body_len, 0) < 0) {
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
    tls_global_init();
    proxy_load();
    char url[1280] = "http://info.cern.ch/";
    if (argc > 1) k93_strlcpy(url, argv[1], sizeof url);

    /* honor HTTP(S)_PROXY env as well (standard Linux logic, per NOTE 3) */
    const char *ep = getenv("HTTPS_PROXY");
    if (!ep) ep = getenv("HTTP_PROXY");
    if (ep && !B.proxy_enable) {
        struct url pu;
        char tmp[1280];
        if (k93_starts_with(ep, "http://")) ep += 7;
        snprintf(tmp, sizeof tmp, "http://%s", ep);
        if (url_parse(tmp, &pu) == 0) {
            k93_strlcpy(B.proxy_host, pu.host, sizeof B.proxy_host);
            B.proxy_port = pu.port;
            B.proxy_enable = 1;
        }
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
