/* k93net.h - HTTP/1.0 + HTTPS client (clean-room).
 *
 * Extracted from the Safari engine so any OS app can fetch URLs: url
 * helpers, proxy support (/etc/k93/proxy.conf), mbedTLS TLS with the
 * system CA store, chunked transfer decoding. All calls are synchronous.
 * Ceiling: GET only, no cookies/keep-alive, one connection per request. */
#ifndef K93NET_H
#define K93NET_H

#include <stddef.h>

struct url { char scheme[16]; char host[256]; int port; char path[1024]; };

int  url_parse(const char *s, struct url *u);   /* 0 ok, -1 not http(s) */
void url_absolutize(const char *base, const char *rel, char *out, size_t cap);

/* IDN (RFC 3492 punycode) - iOS 4.2 "Chinese URL" support: turn a UTF-8 host
 * into its ASCII-compatible "xn--" form for DNS/TLS, and back for display.
 * k93_idn_to_ascii returns 1 = punycoded, 0 = plain ASCII copy, -1 = invalid.
 * k93_idn_to_unicode returns 1 = decoded, 0 = plain ASCII copy, -1 = invalid. */
int  k93_idn_to_ascii(const char *host, char *out, size_t cap);
int  k93_idn_to_unicode(const char *ace, char *out, size_t cap);

void k93net_init(void);           /* load proxy.conf; idempotent */
void k93net_set_ua(const char *ua);   /* default "K93/1.0 (clean-room)" */
/* set proxy programmatically (e.g. from HTTP(S)_PROXY env); ignored when
 * /etc/k93/proxy.conf already enabled one (config has priority) */
void k93net_set_proxy(const char *host, int port);

/* low level: one connection, one request (caller follows redirects) */
struct k93conn;
struct k93conn *k93net_open(const struct url *u, char *err, size_t errlen);
char *k93net_read_http(struct k93conn *c, const struct url *u, int *status,
                       char *err, size_t errlen);   /* body (NUL-terminated) */
void k93net_close(struct k93conn *c);
size_t       k93net_last_len(void);       /* body bytes of last read_http */
const char *k93net_last_location(void);   /* Location header, or "" */

/* high level: GET url, follow redirects (capped); body length via *blen
 * (binary bodies may contain NUL) */
char *k93net_get(const char *url, size_t *blen, int *status,
                 char *err, size_t errlen);

#endif
