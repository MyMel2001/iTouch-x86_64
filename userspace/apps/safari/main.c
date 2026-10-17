/* Safari.app - minimal web browser (clean-room).
 * iOS 4.3.4 shipped MobileSafari (WebKit). This OS cannot reuse WebKit (ARM,
 * and not clean-room), so Safari here is a small HTTP/HTTPS text browser:
 *   - HTTP/1.0 + chunked decoding, HTTPS via mbedTLS with the modern CA store
 *   - honors the HTTP proxy configured in Settings (/etc/k93/proxy.conf)
 *   - renders HTML as readable text, keeps a link list you can follow
 * Ceiling: no JavaScript, no CSS, no images. Text-first web. */
#define _GNU_SOURCE
#include "k93ui.h"
#include "k93util.h"
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
#define MAXLINKS 64
#define MAXPAGE (512 * 1024)

struct url { char scheme[8]; char host[256]; int port; char path[1024]; };

static struct {
    char proxy_host[128];
    int proxy_port;
    int proxy_enable;
    char base[1280];                   /* absolute base for relative links */
    char links[MAXLINKS][1280];
    char linktext[MAXLINKS][80];
    int nlinks;
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

static int url_parse(const char *s, struct url *u)
{
    memset(u, 0, sizeof *u);
    const char *p = strstr(s, "://");
    if (p) {
        size_t n = (size_t)(p - s);
        if (n >= sizeof u->scheme) return -1;
        memcpy(u->scheme, s, n);
        u->scheme[n] = 0;
        p += 3;
    } else {
        k93_strlcpy(u->scheme, "http", sizeof u->scheme);
        p = s;
    }
    /* host[:port][/path] */
    const char *slash = strchr(p, '/');
    const char *colon = strchr(p, ':');
    if (colon && (!slash || colon < slash)) {
        size_t hn = (size_t)(colon - p);
        if (hn >= sizeof u->host) return -1;
        memcpy(u->host, p, hn);
        u->host[hn] = 0;
        u->port = atoi(colon + 1);
    } else {
        size_t hn = slash ? (size_t)(slash - p) : strlen(p);
        if (hn >= sizeof u->host) return -1;
        memcpy(u->host, p, hn);
        u->host[hn] = 0;
        u->port = 0;
    }
    if (!u->port) u->port = strcmp(u->scheme, "https") ? 80 : 443;
    if (slash) k93_strlcpy(u->path, slash, sizeof u->path);
    else k93_strlcpy(u->path, "/", sizeof u->path);
    return u->host[0] ? 0 : -1;
}

static void url_absolutize(const char *base, const char *rel, char *out, size_t cap)
{
    if (!strncmp(rel, "http://", 7) || !strncmp(rel, "https://", 8)) {
        k93_strlcpy(out, rel, cap);
        return;
    }
    struct url b;
    if (url_parse(base, &b) < 0) { k93_strlcpy(out, rel, cap); return; }
    if (rel[0] == '/') {
        snprintf(out, cap, "%s://%s%s", b.scheme, b.host, rel);
        return;
    }
    /* relative to current directory */
    char dir[1280];
    k93_strlcpy(dir, b.path, sizeof dir);
    char *slash = strrchr(dir, '/');
    if (slash) slash[1] = 0;
    else k93_strlcpy(dir, "/", sizeof dir);
    snprintf(out, cap, "%s://%s%s%s", b.scheme, b.host, dir, rel);
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
    if (B.proxy_enable && B.proxy_host[0] && !strcmp(u->scheme, "http")) {
        n = snprintf(req, sizeof req,
                     "GET %s://%s:%d%s HTTP/1.0\r\n"
                     "Host: %s\r\nUser-Agent: K93Safari/1.0 (clean-room)\r\n"
                     "Accept: text/html,*/*\r\nConnection: close\r\n\r\n",
                     u->scheme, u->host, u->port, u->path, u->host);
    } else {
        n = snprintf(req, sizeof req,
                     "GET %s HTTP/1.0\r\n"
                     "Host: %s\r\nUser-Agent: K93Safari/1.0 (clean-room)\r\n"
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
    return buf;
}

/* ---------------------------------------------------------------- html->text */

static void decode_entities_inplace(char *s)
{
    for (char *p = s; *p; p++) {
        if (*p != '&') continue;
        if (!strncmp(p, "&amp;", 5)) { memmove(p, p + 4, strlen(p + 4) + 1); }
        else if (!strncmp(p, "&lt;", 4)) { memmove(p, p + 3, strlen(p + 3) + 1); }
        else if (!strncmp(p, "&gt;", 4)) { memmove(p, p + 3, strlen(p + 3) + 1); }
        else if (!strncmp(p, "&nbsp;", 6)) { memmove(p, p + 5, strlen(p + 5) + 1); }
        else if (!strncmp(p, "&quot;", 6)) { *p = '"'; memmove(p + 1, p + 6, strlen(p + 6) + 1); }
    }
}

static char *html_to_text(const char *html, char *title_out, size_t titlecap)
{
    size_t cap = strlen(html) + 1;
    char *out = k93_xmalloc(cap);
    size_t o = 0;
    int skip = 0;                       /* inside script/style */
    B.nlinks = 0;
    const char *p = html;

    /* title */
    if (title_out && titlecap) {
        title_out[0] = 0;
        const char *t = strcasestr(html, "<title");
        if (t) {
            t = strchr(t, '>');
            if (t) {
                const char *e = strchr(t, '<');
                if (e && (size_t)(e - t) < titlecap - 1) {
                    memcpy(title_out, t + 1, (size_t)(e - t - 1));
                    title_out[e - t - 1] = 0;
                    decode_entities_inplace(title_out);
                }
            }
        }
    }

    char curhref[1280] = "";
    char curlink[80] = "";
    int in_a = 0;

    while (*p && o + 8 < cap) {
        if (*p == '<') {
            const char *e = strchr(p, '>');
            if (!e) break;
            size_t taglen = (size_t)(e - p) + 1;
            char tag[128];
            size_t tl = taglen < sizeof tag - 1 ? taglen : sizeof tag - 1;
            memcpy(tag, p, tl);
            tag[tl] = 0;
            for (char *q = tag; *q; q++) *q = (char)tolower((unsigned char)*q);

            if (!strncmp(tag, "<script", 7) || !strncmp(tag, "<style", 6)) skip = 1;
            if (!strncmp(tag, "</script", 8) || !strncmp(tag, "</style", 7)) skip = 0;
            if (!strncmp(tag, "<br", 3) || !strncmp(tag, "<p", 2) ||
                !strncmp(tag, "<div", 4) || !strncmp(tag, "<li", 3) ||
                !strncmp(tag, "<tr", 3) || !strncmp(tag, "<h1", 3) ||
                !strncmp(tag, "<h2", 3) || !strncmp(tag, "<h3", 3)) {
                if (o && out[o - 1] != '\n') out[o++] = '\n';
            }
            if (!strncmp(tag, "<a ", 3)) {
                const char *h = strstr(tag, "href=\"");
                if (h && B.nlinks < MAXLINKS) {
                    k93_strlcpy(curhref, h + 6, sizeof curhref);
                    char *q = strchr(curhref, '"');
                    if (q) *q = 0;
                    url_absolutize(B.base, curhref, B.links[B.nlinks], sizeof B.links[0]);
                    B.linktext[B.nlinks][0] = 0;
                    in_a = 1;
                }
            }
            if (!strncmp(tag, "</a", 3) && in_a) {
                if (B.nlinks < MAXLINKS) {
                    if (curlink[0]) {
                        k93_strlcpy(B.linktext[B.nlinks], curlink, sizeof B.linktext[0]);
                        B.nlinks++;
                    }
                }
                curlink[0] = 0;
                in_a = 0;
            }
            p = e + 1;
            continue;
        }
        if (!skip) {
            char c = *p;
            if (c == '\n' || c == '\r' || c == '\t') c = ' ';
            out[o++] = c;
            if (in_a && c != ' ' && strlen(curlink) < sizeof curlink - 1) {
                size_t l = strlen(curlink);
                curlink[l] = c;
                curlink[l + 1] = 0;
            }
        }
        p++;
    }
    out[o] = 0;
    decode_entities_inplace(out);

    /* collapse spaces and blank lines */
    char *clean = k93_xmalloc(cap);
    size_t co = 0;
    int lastsp = 1, lastnl = 1;
    for (size_t i = 0; out[i] && co + 1 < cap; i++) {
        char c = out[i];
        if (c == ' ') { if (lastsp) continue; lastsp = 1; }
        else lastsp = 0;
        if (c == '\n') { if (lastnl) continue; lastnl = 1; }
        else lastnl = 0;
        clean[co++] = c;
    }
    clean[co] = 0;
    free(out);
    return clean;
}

/* ---------------------------------------------------------------- app */

static int browse(const char *url_in)
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
    if (status >= 300 && status < 400) {
        /* naive redirect support */
        char *loc = strstr(raw, "\nLocation: ");
        if (!loc) loc = strstr(raw, "\nlocation: ");
        if (loc) {
            char target[1280];
            char *s = loc + 11;
            char *e = strstr(s, "\r\n");
            size_t n = e ? (size_t)(e - s) : strlen(s);
            if (n >= sizeof target) n = sizeof target - 1;
            memcpy(target, s, n);
            target[n] = 0;
            free(raw);
            char abs[1280];
            url_absolutize(url, target, abs, sizeof abs);
            return browse(abs);
        }
    }
    if (status >= 400) {
        char m[128];
        snprintf(m, sizeof m, "Server returned HTTP %d", status);
        ui_msgbox("Safari", m);
    }

    char title[96] = "";
    char *text = html_to_text(raw, title, sizeof title);
    free(raw);

    char header[256];
    snprintf(header, sizeof header, "%s\n%s\n\n", url, title[0] ? title : "(no title)");
    size_t hl = strlen(header), tl = strlen(text);
    char *page = k93_xmalloc(hl + tl + 1);
    memcpy(page, header, hl);
    memcpy(page + hl, text, tl + 1);
    free(text);

    ui_textview(title[0] ? title : "Safari", page);
    free(page);

    if (B.nlinks) {
        const char *litems[MAXLINKS + 2];
        for (int i = 0; i < B.nlinks; i++)
            litems[i] = B.linktext[i][0] ? B.linktext[i] : B.links[i];
        litems[B.nlinks] = "(close)";
        int sel = ui_menu("Links on this page", litems, B.nlinks + 1, 0);
        if (sel >= 0 && sel < B.nlinks) return browse(B.links[sel]);
    }
    return 0;
}

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

    for (;;) {
        if (ui_input("Address (URL)", url, sizeof url) != 0) break;
        browse(url);
        proxy_load();
    }
    ui_shutdown();
    return 0;
}
