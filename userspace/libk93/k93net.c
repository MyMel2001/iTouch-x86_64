/* k93net.c - HTTP/1.0 + HTTPS client (clean-room), extracted from the
 * Safari engine for use by any OS app.
 *   - HTTP/1.0 + chunked decoding, HTTPS via mbedTLS with the system CA
 *     store (/etc/ssl/certs/ca-certificates.crt)
 *   - honors the HTTP proxy in /etc/k93/proxy.conf (CONNECT tunnel for TLS)
 * Ceiling: GET only, no cookies/keep-alive, synchronous. */
#define _GNU_SOURCE
#include "k93net.h"
#include "k93util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>
#include <unistd.h>
#include <netdb.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/error.h>

#define CACERT "/etc/ssl/certs/ca-certificates.crt"
#define MAXPAGE (8 * 1024 * 1024)

static size_t body_len;                /* body bytes of last read_http
                                          (binary bodies may contain NUL) */
static char location[1280];            /* Location header of last read_http */
static char ua[96] = "K93/1.0 (clean-room)";
static struct {
    char proxy_host[128];
    int proxy_port;
    int proxy_enable;
} PX;
static int inited;

void k93net_set_ua(const char *s)
{
    if (s) k93_strlcpy(ua, s, sizeof ua);
}

void k93net_set_proxy(const char *host, int port)
{
    k93net_init();
    if (PX.proxy_enable) return;         /* /etc/k93/proxy.conf has priority */
    k93_strlcpy(PX.proxy_host, host ? host : "", sizeof PX.proxy_host);
    PX.proxy_port = port;
    PX.proxy_enable = host && host[0] && port > 0;
}

void k93net_init(void)
{
    if (inited) return;
    inited = 1;
    PX.proxy_host[0] = 0; PX.proxy_port = 0; PX.proxy_enable = 0;
    size_t len = 0;
    char *c = k93_read_file("/etc/k93/proxy.conf", &len);
    if (!c) return;
    for (char *line = strtok(c, "\n"); line; line = strtok(NULL, "\n")) {
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char *k = k93_trim(line), *v = k93_trim(eq + 1);
        if (!strcmp(k, "PROXY_HOST")) k93_strlcpy(PX.proxy_host, v, sizeof PX.proxy_host);
        else if (!strcmp(k, "PROXY_PORT")) PX.proxy_port = atoi(v);
        else if (!strcmp(k, "PROXY_ENABLE")) PX.proxy_enable = atoi(v);
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
    if (!strncmp(rel, "http://", 7) || !strncmp(rel, "https://", 8) ||
        !strncmp(rel, "file://", 7)) {
        k93_strlcpy(out, rel, cap);
        return;
    }
    if (!strncmp(rel, "javascript:", 11) || !strncmp(rel, "mailto:", 7) ||
        !strncmp(rel, "data:", 5)) {
        k93_strlcpy(out, rel, cap);
        return;
    }
    if (rel[0] == '#') {                  /* fragment-only: base path + ref */
        k93_strlcpy(out, base, cap);
        char *hf = strchr(out, '#');
        if (hf) *hf = 0;
        size_t n = strlen(out);
        snprintf(out + n, cap - n, "%s", rel);
        return;
    }
    if (!strncmp(base, "file://", 7)) {  /* local file: plain dir join */
        char dir[1280];
        if (rel[0] == '/') {
            snprintf(out, cap, "file://%s", rel);
            return;
        }
        k93_strlcpy(dir, base + 7, sizeof dir);
        char *dslash = strrchr(dir, '/');
        if (dslash) dslash[1] = 0;
        else k93_strlcpy(dir, "/", sizeof dir);
        snprintf(out, cap, "file://%s%s", dir, rel);
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

/* --------------------------------------------------------------------- IDN */
/* RFC 3492 punycode, clean-room. iOS 4.2 shipped "Chinese URL" support in
 * Safari: internationalized hostnames must be punycoded (xn--...) before DNS
 * and TLS, and non-ASCII request paths percent-encoded. */

enum { PUNY_BASE = 36, PUNY_TMIN = 1, PUNY_TMAX = 26, PUNY_SKEW = 38,
       PUNY_DAMP = 700, PUNY_BIAS = 72, PUNY_N0 = 128, PUNY_MAXCP = 256 };

static int puny_adapt(int delta, int numpoints, int first)
{
    delta = first ? delta / PUNY_DAMP : delta / 2;
    delta += delta / numpoints;
    int k = 0;
    while (delta > ((PUNY_BASE - PUNY_TMIN) * PUNY_TMAX) / 2) {
        delta /= PUNY_BASE - PUNY_TMIN;
        k += PUNY_BASE;
    }
    return k + (((PUNY_BASE - PUNY_TMIN + 1) * delta) / (delta + PUNY_SKEW));
}

static int u32_decode(const char *s, size_t n, uint32_t *out)
{
    size_t i = 0;
    int o = 0;
    while (i < n && o < PUNY_MAXCP) {
        unsigned c = (unsigned char)s[i];
        uint32_t cp;
        int extra;
        if (c < 0x80) { cp = c; extra = 0; }
        else if ((c & 0xE0) == 0xC0) { cp = c & 0x1Fu; extra = 1; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0Fu; extra = 2; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07u; extra = 3; }
        else return -1;
        if (i + (size_t)extra >= n) return -1;
        for (int k = 0; k < extra; k++) {
            unsigned cc = (unsigned char)s[i + 1 + k];
            if ((cc & 0xC0) != 0x80) return -1;
            cp = (cp << 6) | (cc & 0x3Fu);
        }
        if (cp > 0x10FFFF) return -1;
        i += (size_t)extra + 1;
        out[o++] = (uint32_t)cp;
    }
    return o;
}

static int u32_encode(const uint32_t *in, int n, char *out, size_t cap)
{
    size_t o = 0;
    for (int i = 0; i < n; i++) {
        uint32_t cp = in[i];
        if (cp < 0x80) {
            if (o + 1 >= cap) return -1;
            out[o++] = (char)cp;
        } else {
            int extra = cp < 0x800 ? 1 : (cp < 0x10000 ? 2 : 3);
            if (o + (size_t)extra + 1 >= cap) return -1;
            static const unsigned char m[4] = { 0, 0xC0, 0xE0, 0xF0 };
            out[o++] = (char)(m[extra] | (cp >> (6 * extra)));
            for (int k = extra - 1; k >= 0; k--)
                out[o++] = (char)(0x80 | ((cp >> (6 * k)) & 0x3F));
        }
    }
    out[o] = 0;
    return (int)o;
}

static int puny_encode(const uint32_t *in, int n, char *out, size_t cap)
{
    size_t o = 0;
    int nn = PUNY_N0, delta = 0, bias = PUNY_BIAS;
    int h = 0;
    for (int i = 0; i < n; i++)
        if (in[i] < 128) {
            if (o + 1 >= cap) return -1;
            out[o++] = (char)in[i];
            h++;
        }
    int b = h;
    if (b > 0) {                          /* delimiter after the literal part */
        if (o + 1 >= cap) return -1;
        out[o++] = '-';
    }
    while (h < n) {
        uint32_t m = 0x7FFFFFFF;
        for (int i = 0; i < n; i++) if (in[i] >= (uint32_t)nn && in[i] < m) m = in[i];
        delta += (int)((m - (uint32_t)nn) * (uint32_t)(h + 1));
        nn = (int)m;
        for (int i = 0; i < n; i++) {
            if (in[i] < (uint32_t)nn) { delta++; continue; }
            if (in[i] != (uint32_t)nn) continue;
            long q = delta;
            for (int k = PUNY_BASE;; k += PUNY_BASE) {
                int t = k <= bias ? PUNY_TMIN : (k >= bias + PUNY_TMAX ? PUNY_TMAX : k - bias);
                if (q < t) break;
                if (o + 1 >= cap) return -1;
                out[o++] = "abcdefghijklmnopqrstuvwxyz0123456789"[(t + (int)((q - t) % (PUNY_BASE - t))) % 36];
                q = (q - t) / (PUNY_BASE - t);
            }
            if (o + 1 >= cap) return -1;
            out[o++] = "abcdefghijklmnopqrstuvwxyz0123456789"[q % 36];
            bias = puny_adapt(delta, h + 1, h == b);
            delta = 0;
            h++;
        }
        delta++;
        nn++;
    }
    out[o] = 0;
    return (int)o;
}

static int puny_decode(const char *s, size_t n, uint32_t *out, int max)
{
    int o = 0;
    /* literal (ASCII) part before the last '-'; none if there is no '-' */
    const char *dash = NULL;
    for (size_t i = 0; i < n; i++) if (s[i] == '-') dash = s + i;
    size_t basic = dash ? (size_t)(dash - s) : 0;
    for (size_t i = 0; i < basic; i++) {
        if (o >= max) return -1;
        out[o++] = (unsigned char)s[i];
    }
    int nn = PUNY_N0, i = 0, bias = PUNY_BIAS;
    size_t p = dash ? basic + 1 : 0;      /* skip the delimiter */
    while (p < n) {
        int oldi = i, w = 1;
        for (int k = PUNY_BASE;; k += PUNY_BASE) {
            if (p >= n) return -1;
            unsigned char c = (unsigned char)s[p++];
            int digit;
            if (c >= 'a' && c <= 'z') digit = c - 'a';
            else if (c >= 'A' && c <= 'Z') digit = c - 'A';
            else if (c >= '0' && c <= '9') digit = c - '0' + 26;
            else return -1;
            i += digit * w;
            int t = k <= bias ? PUNY_TMIN : (k >= bias + PUNY_TMAX ? PUNY_TMAX : k - bias);
            if (digit < t) break;
            w *= PUNY_BASE - t;
        }
        bias = puny_adapt(i - oldi, o + 1, oldi == 0);
        nn += i / (o + 1);
        i %= (o + 1);
        if (o >= max) return -1;
        memmove(out + i + 1, out + i, (size_t)(o - i) * sizeof *out);
        out[i++] = (uint32_t)nn;
        o++;
    }
    return o;
}

/* One dotted host label (UTF-8 <-> punycode). */
static int idn_label_ascii(const char *label, char *out, size_t cap)
{
    uint32_t cp[PUNY_MAXCP];
    int n = u32_decode(label, strlen(label), cp);
    if (n < 0) return -1;
    int has_uni = 0;
    for (int i = 0; i < n; i++) if (cp[i] >= 128) { has_uni = 1; break; }
    if (!has_uni) { k93_strlcpy(out, label, cap); return 0; }
    char body[512];
    if (puny_encode(cp, n, body, sizeof body) < 0) return -1;
    if (snprintf(out, cap, "xn--%s", body) >= (int)cap) return -1;
    return 1;
}

static int idn_label_unicode(const char *label, char *out, size_t cap)
{
    if (strncasecmp(label, "xn--", 4)) { k93_strlcpy(out, label, cap); return 0; }
    uint32_t cp[PUNY_MAXCP];
    int n = puny_decode(label + 4, strlen(label + 4), cp, PUNY_MAXCP);
    if (n < 0) return -1;
    return u32_encode(cp, n, out, cap) < 0 ? -1 : 1;
}

int k93_idn_to_ascii(const char *host, char *out, size_t cap)
{
    if (!host || !cap) return -1;
    size_t o = 0;
    int any = 0;
    const char *p = host;
    for (;;) {
        const char *dot = strchr(p, '.');
        size_t ll = dot ? (size_t)(dot - p) : strlen(p);
        char lab[128];
        if (ll >= sizeof lab) return -1;
        memcpy(lab, p, ll);
        lab[ll] = 0;
        char enc[192];
        if (idn_label_ascii(lab, enc, sizeof enc) < 0) return -1;
        if (o + strlen(enc) + 2 >= cap) return -1;
        o += (size_t)sprintf(out + o, "%s%s", enc, dot ? "." : "");
        if (strcmp(enc, lab)) any = 1;
        if (!dot) break;
        p = dot + 1;
    }
    return any;
}

int k93_idn_to_unicode(const char *ace, char *out, size_t cap)
{
    if (!ace || !cap) return -1;
    size_t o = 0;
    int any = 0;
    const char *p = ace;
    for (;;) {
        const char *dot = strchr(p, '.');
        size_t ll = dot ? (size_t)(dot - p) : strlen(p);
        char lab[128];
        if (ll >= sizeof lab) return -1;
        memcpy(lab, p, ll);
        lab[ll] = 0;
        char dec[192];
        if (idn_label_unicode(lab, dec, sizeof dec) < 0) return -1;
        if (o + strlen(dec) + 2 >= cap) return -1;
        o += (size_t)sprintf(out + o, "%s%s", dec, dot ? "." : "");
        if (strcmp(dec, lab)) any = 1;
        if (!dot) break;
        p = dot + 1;
    }
    return any;
}

/* Percent-encode a request target: bytes >= 0x80, space and the URL
 * delimiters get %XX; an existing %XX sequence is left alone. */
static void url_enc_path(const char *in, char *out, size_t cap)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p; p++) {
        unsigned char c = *p;
        int enc = (c >= 0x80 || c <= 0x20 || c == '"' || c == '<' || c == '>' ||
                   c == '`' || c == '{' || c == '}' || c == '|' || c == '\\' ||
                   c == '^' || c == 0x7F);
        if (enc) {
            if (o + 4 >= cap) break;
            out[o++] = '%';
            out[o++] = hex[c >> 4];
            out[o++] = hex[c & 15];
        } else {
            if (o + 2 >= cap) break;
            out[o++] = (char)c;
        }
    }
    out[o] = 0;
}

/* ---------------------------------------------------------------- transport */

struct k93conn {
    int fd;                            /* plain socket (also used pre-tunnel) */
    mbedtls_ssl_context ssl;
    int tls;
};

static int dial(const char *host, int port)
{
    char portstr[16];
    snprintf(portstr, sizeof portstr, "%d", port);
    char ace[256];                        /* IDN hosts resolve as xn--... */
    if (k93_idn_to_ascii(host, ace, sizeof ace) < 0)
        k93_strlcpy(ace, host, sizeof ace);
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(ace, portstr, &hints, &res) != 0 || !res) return -1;
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
    const char *pers = "k93-net";
    if (mbedtls_ctr_drbg_seed(&g_ctr, mbedtls_entropy_func, &g_entropy,
                              (const unsigned char *)pers, strlen(pers)) != 0)
        return;
    if (mbedtls_x509_crt_parse_file(&g_ca, CACERT) < 0)
        k93_log("k93net", "warning: cannot load CA bundle " CACERT);
    g_tls_ready = 1;
}

struct k93conn *k93net_open(const struct url *u, char *err, size_t errlen)
{
    k93net_init();
    struct k93conn *c = calloc(1, sizeof *c);
    if (!c) { snprintf(err, errlen, "oom"); return NULL; }
    c->fd = -1;
    int use_tls = !strcmp(u->scheme, "https");
    if (use_tls) tls_global_init();      /* lazy: first https request */

    if (PX.proxy_enable && PX.proxy_host[0])
        c->fd = dial(PX.proxy_host, PX.proxy_port);
    else
        c->fd = dial(u->host, u->port);
    if (c->fd < 0) { snprintf(err, errlen, "cannot connect"); free(c); return NULL; }

    if (use_tls) {
        if (!g_tls_ready) {
            snprintf(err, errlen, "TLS not initialized (CA bundle missing?)");
            close(c->fd); free(c); return NULL;
        }
        /* tunnel through the proxy first when one is configured */
        char ace_host[256];
        if (k93_idn_to_ascii(u->host, ace_host, sizeof ace_host) < 0)
            k93_strlcpy(ace_host, u->host, sizeof ace_host);
        if (PX.proxy_enable && PX.proxy_host[0]) {
            char req[512];
            int n = snprintf(req, sizeof req, "CONNECT %s:%d HTTP/1.0\r\n\r\n", ace_host, u->port);
            if (write(c->fd, req, (size_t)n) != n) {
                snprintf(err, errlen, "proxy write failed");
                close(c->fd); free(c); return NULL;
            }
            char resp[512];
            ssize_t r = read(c->fd, resp, sizeof resp - 1);
            if (r <= 0 || !strstr(resp, " 200 ")) {
                snprintf(err, errlen, "proxy CONNECT failed");
                close(c->fd); free(c); return NULL;
            }
        }
        mbedtls_ssl_init(&c->ssl);
        static mbedtls_ssl_config conf;
        mbedtls_ssl_config_init(&conf);
        mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
                                    MBEDTLS_SSL_PRESET_DEFAULT);
        mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_REQUIRED);
        mbedtls_ssl_conf_ca_chain(&conf, &g_ca, NULL);
        mbedtls_ssl_conf_rng(&conf, mbedtls_ctr_drbg_random, &g_ctr);
        if (mbedtls_ssl_setup(&c->ssl, &conf) != 0) {
            snprintf(err, errlen, "TLS setup failed");
            close(c->fd); free(c); return NULL;
        }
        mbedtls_ssl_set_hostname(&c->ssl, ace_host);
        mbedtls_net_context nb;
        mbedtls_net_init(&nb);
        nb.fd = c->fd;
        mbedtls_ssl_set_bio(&c->ssl, &nb, mbedtls_net_send, mbedtls_net_recv, NULL);
        int rc;
        while ((rc = mbedtls_ssl_handshake(&c->ssl)) != 0) {
            if (rc != MBEDTLS_ERR_SSL_WANT_READ && rc != MBEDTLS_ERR_SSL_WANT_WRITE) {
                char ebuf[96];
                mbedtls_strerror(rc, ebuf, sizeof ebuf);
                snprintf(err, errlen, "TLS handshake failed: %s", ebuf);
                k93_log("k93net", "TLS handshake failed: %s", ebuf);
                mbedtls_ssl_free(&c->ssl);
                close(c->fd);
                free(c);
                return NULL;
            }
        }
        c->tls = 1;
    }
    return c;
}

static ssize_t conn_read(struct k93conn *c, void *buf, size_t n)
{
    if (c->tls) {
        int r = mbedtls_ssl_read(&c->ssl, buf, n);
        if (r < 0 && r != MBEDTLS_ERR_SSL_WANT_READ && r != MBEDTLS_ERR_SSL_WANT_WRITE) return -1;
        return r;
    }
    return read(c->fd, buf, n);
}

static int conn_write(struct k93conn *c, const void *buf, size_t n)
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

void k93net_close(struct k93conn *c)
{
    if (!c) return;
    if (c->tls) { mbedtls_ssl_close_notify(&c->ssl); mbedtls_ssl_free(&c->ssl); }
    if (c->fd >= 0) { close(c->fd); c->fd = -1; }
    free(c);
}

/* ---------------------------------------------------------------- fetch */

size_t k93net_last_len(void) { return body_len; }
const char *k93net_last_location(void) { return location; }

char *k93net_read_http(struct k93conn *c, const struct url *u, const char *method,
                       const char *body, size_t bodylen, const char *ctype,
                       int *status, char *err, size_t errlen)
{
    char req[4096];
    char ace_host[256];
    char enc_path[3072];
    int n;
    const char *m = (method && *method) ? method : "GET";
    int with_body = body && bodylen > 0;
    location[0] = 0;
    if (k93_idn_to_ascii(u->host, ace_host, sizeof ace_host) < 0)
        k93_strlcpy(ace_host, u->host, sizeof ace_host);
    url_enc_path(u->path, enc_path, sizeof enc_path);
    const char *hdr_extra = "";
    char extra[160];
    if (with_body)
        snprintf(extra, sizeof extra, "Content-Type: %s\r\nContent-Length: %zu\r\n",
                 ctype && *ctype ? ctype : "application/x-www-form-urlencoded",
                 bodylen);
    else
        extra[0] = 0;
    hdr_extra = extra;
    if (PX.proxy_enable && PX.proxy_host[0] && !strcmp(u->scheme, "http")) {
        n = snprintf(req, sizeof req,
                     "%s %s://%s:%d%s HTTP/1.0\r\n"
                     "Host: %s\r\nUser-Agent: %s\r\n"
                     "Accept: text/html,*/*\r\n%sConnection: close\r\n\r\n",
                     m, u->scheme, ace_host, u->port, enc_path, ace_host, ua,
                     hdr_extra);
    } else {
        n = snprintf(req, sizeof req,
                     "%s %s HTTP/1.0\r\n"
                     "Host: %s\r\nUser-Agent: %s\r\n"
                     "Accept: text/html,*/*\r\n%sConnection: close\r\n\r\n",
                     m, enc_path, ace_host, ua, hdr_extra);
    }
    if (n < 0 || n >= (int)sizeof req) {
        snprintf(err, errlen, "request too long");
        return NULL;
    }
    if (conn_write(c, req, (size_t)n) < 0) { snprintf(err, errlen, "write failed"); return NULL; }
    if (with_body && conn_write(c, body, bodylen) < 0) {
        snprintf(err, errlen, "write failed");
        return NULL;
    }

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
                if (ln >= sizeof location) ln = sizeof location - 1;
                memcpy(location, s, ln);
                location[ln] = 0;
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
    body_len = len;
    return buf;
}

/* GET url, follow redirects (capped), body + length */
char *k93net_get(const char *url, size_t *blen, int *status,
                 char *err, size_t errlen)
{
    static int depth;
    struct url u;
    *status = 0;
    if (blen) *blen = 0;
    if (url_parse(url, &u) < 0) {
        snprintf(err, errlen, "bad url");
        return NULL;
    }
    struct k93conn *c = k93net_open(&u, err, errlen);
    if (!c) return NULL;
    char *raw = k93net_read_http(c, &u, "GET", NULL, 0, NULL, status, err, errlen);
    k93net_close(c);
    if (blen) *blen = k93net_last_len();
    if (raw && *status >= 300 && *status < 400 && location[0] && depth < 8) {
        char abs[1280];
        url_absolutize(url, location, abs, sizeof abs);
        free(raw);
        depth++;
        raw = k93net_get(abs, blen, status, err, errlen);
        depth--;
        return raw;
    }
    return raw;
}

/* any method (POST/PUT/...); body/ctype optional; follows redirects (capped) */
char *k93net_request(const char *method, const char *url,
                     const char *body, size_t bodylen, const char *ctype,
                     size_t *blen, int *status, char *err, size_t errlen)
{
    static int depth;
    struct url u;
    *status = 0;
    if (blen) *blen = 0;
    if (url_parse(url, &u) < 0) {
        snprintf(err, errlen, "bad url");
        return NULL;
    }
    struct k93conn *c = k93net_open(&u, err, errlen);
    if (!c) return NULL;
    char *raw = k93net_read_http(c, &u, method, body, bodylen, ctype,
                                 status, err, errlen);
    k93net_close(c);
    if (blen) *blen = k93net_last_len();
    if (raw && *status >= 300 && *status < 400 && location[0] && depth < 8) {
        char abs[1280];
        url_absolutize(url, location, abs, sizeof abs);
        free(raw);
        depth++;
        raw = k93net_request(method, abs, body, bodylen, ctype,
                             blen, status, err, errlen);
        depth--;
        return raw;
    }
    return raw;
}
