/* k93plist.c - minimal XML plist parser, clean-room.
 * Written from the public Apple Open Source "CFPropertyList" *format* docs and
 * observed document structure in the supplied iOS 4.3.4 rootfs; no Apple code. */
#define _GNU_SOURCE
#include "k93plist.h"
#include "k93util.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

struct k93_plist {
    k93_plist_type type;
    char *str;                       /* STRING value / DICT key of child */
    long  ival;
    double rval;
    int   bval;
    struct k93_plist **items;        /* DICT: alternating key,value ; ARRAY: elements */
    int   nitems, cap;
};

static void plist_err(char *eb, size_t el, const char *msg, const char *at)
{
    if (eb && el) {
        char near[32] = {0};
        if (at) { strncpy(near, at, 24); near[24] = 0; }
        snprintf(eb, el, "%s (near \"%.24s\")", msg, near);
    }
}

static k93_plist *node_new(k93_plist_type t)
{
    k93_plist *p = k93_xmalloc(sizeof *p);
    p->type = t;
    return p;
}

static void node_add(k93_plist *parent, k93_plist *child)
{
    if (parent->nitems == parent->cap) {
        parent->cap = parent->cap ? parent->cap * 2 : 8;
        parent->items = realloc(parent->items, sizeof(*parent->items) * (size_t)parent->cap);
        if (!parent->items) exit(201);
    }
    parent->items[parent->nitems++] = child;
}

void k93_plist_free(k93_plist *p)
{
    if (!p) return;
    free(p->str);
    for (int i = 0; i < p->nitems; i++) k93_plist_free(p->items[i]);
    free(p->items);
    free(p);
}

static char *decode_entities(const char *s, size_t n)
{
    char *out = k93_xmalloc(n + 1);
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '&') {
            if (!strncmp(s + i, "&amp;", 5))  { out[o++] = '&'; i += 4; }
            else if (!strncmp(s + i, "&lt;", 4))  { out[o++] = '<'; i += 3; }
            else if (!strncmp(s + i, "&gt;", 4))  { out[o++] = '>'; i += 3; }
            else if (!strncmp(s + i, "&quot;", 6)) { out[o++] = '"'; i += 5; }
            else if (!strncmp(s + i, "&apos;", 6)) { out[o++] = '\''; i += 5; }
            else out[o++] = s[i];
        } else out[o++] = s[i];
    }
    out[o] = 0;
    return out;
}

static const char *skip_ws(const char *p, const char *end)
{
    while (p < end) {
        if (*p == '<' && p + 3 < end && !strncmp(p, "<!--", 4)) {
            const char *e = strstr(p, "-->");
            p = e ? e + 3 : end;
            continue;
        }
        if (!isspace((unsigned char)*p)) break;
        p++;
    }
    return p;
}

/* returns tag name after '<'; sets *attrs to attribute text (not decoded) */
static const char *next_tag(const char **pp, const char *end, char *tag, size_t taglen,
                            const char **attrs, int *closing)
{
    const char *p = skip_ws(*pp, end);
    if (p >= end || *p != '<') { *pp = p; return NULL; }
    p++;
    *closing = (*p == '/');
    if (*closing) p++;
    while (*p && isspace((unsigned char)*p)) p++;
    size_t t = 0;
    while (*p && !isspace((unsigned char)*p) && *p != '>' && *p != '/' && t + 1 < taglen)
        tag[t++] = *p++;
    tag[t] = 0;
    const char *a = p;
    while (*p && *p != '>') p++;
    if (*attrs) *attrs = a;
    if (p < end) p++;                       /* consume '>' */
    *pp = p;
    return t ? tag : NULL;
}

static int tag_is(const char *tag, const char *name) { return !strcmp(tag, name); }

static k93_plist *parse_value(const char **pp, const char *end, char *eb, size_t el);

static k93_plist *parse_dict(const char **pp, const char *end, char *eb, size_t el)
{
    k93_plist *d = node_new(K93_PLIST_DICT);
    for (;;) {
        const char *tagstart = skip_ws(*pp, end);
        char tag[64]; const char *attrs = NULL; int closing = 0;
        const char *q = tagstart;
        if (!next_tag(&q, end, tag, sizeof tag, &attrs, &closing)) {
            plist_err(eb, el, "unterminated dict", *pp);
            goto fail;
        }
        if (closing) {
            if (!tag_is(tag, "dict")) { plist_err(eb, el, "unexpected close tag in dict", *pp); goto fail; }
            *pp = q;
            return d;
        }
        if (tag_is(tag, "key")) {
            const char *s = q, *e = strstr(s, "</key>");
            if (!e) { plist_err(eb, el, "unterminated key", s); goto fail; }
            k93_plist *k = node_new(K93_PLIST_STRING);
            k->str = decode_entities(s, (size_t)(e - s));
            node_add(d, k);
            *pp = e + 6;
        } else if (tag_is(tag, "!DOCTYPE") || tag_is(tag, "?xml")) {
            *pp = q;
        } else {
            *pp = tagstart;                 /* reparse this tag as a value */
            k93_plist *v = parse_value(pp, end, eb, el);
            if (!v) goto fail;
            if (d->nitems % 2 == 0) { plist_err(eb, el, "value without key in dict", *pp); k93_plist_free(v); goto fail; }
            node_add(d, v);
        }
    }
fail:
    k93_plist_free(d);
    return NULL;
}

static k93_plist *parse_array(const char **pp, const char *end, char *eb, size_t el)
{
    k93_plist *a = node_new(K93_PLIST_ARRAY);
    for (;;) {
        const char *tagstart = skip_ws(*pp, end);
        char tag[64]; const char *attrs = NULL; int closing = 0;
        const char *q = tagstart;
        if (!next_tag(&q, end, tag, sizeof tag, &attrs, &closing)) {
            plist_err(eb, el, "unterminated array", *pp);
            goto fail;
        }
        if (closing) {
            if (!tag_is(tag, "array")) { plist_err(eb, el, "unexpected close tag in array", *pp); goto fail; }
            *pp = q;
            return a;
        }
        *pp = tagstart;                     /* reparse this tag as a value */
        k93_plist *v = parse_value(pp, end, eb, el);
        if (!v) goto fail;
        node_add(a, v);
    }
fail:
    k93_plist_free(a);
    return NULL;
}

static k93_plist *parse_value(const char **pp, const char *end, char *eb, size_t el)
{
    char tag[64]; const char *attrs = NULL; int closing = 0;
    const char *tagstart = skip_ws(*pp, end);
    const char *q = tagstart;
    if (!next_tag(&q, end, tag, sizeof tag, &attrs, &closing)) {
        plist_err(eb, el, "expected value", *pp);
        return NULL;
    }
    if (closing) { plist_err(eb, el, "unexpected closing tag", tagstart); return NULL; }

    if (tag_is(tag, "dict"))   { *pp = q; return parse_dict(pp, end, eb, el); }
    if (tag_is(tag, "array"))  { *pp = q; return parse_array(pp, end, eb, el); }

    /* self-closing tags such as <true/> or <false/> carry no content */
    {
        const char *gt = strchr(tagstart, '>');
        int self_close = (gt && gt > tagstart && *(gt - 1) == '/');
        if (self_close) {
            *pp = q;
            if (tag_is(tag, "true") || tag_is(tag, "false")) {
                k93_plist *b = node_new(K93_PLIST_BOOL);
                b->bval = tag_is(tag, "true");
                return b;
            }
            plist_err(eb, el, "self-closing element with content required", tag);
            return NULL;
        }
    }

    *pp = q;
    /* simple-content element: read until matching close tag */
    const char *c = strstr(*pp, "</");
    if (!c) { plist_err(eb, el, "unterminated value", tagstart); return NULL; }
    const char *close_end = strchr(c, '>');
    if (!close_end) { plist_err(eb, el, "malformed close tag", c); return NULL; }

    k93_plist *p = NULL;
    if (tag_is(tag, "string")) {
        p = node_new(K93_PLIST_STRING);
        p->str = decode_entities(*pp, (size_t)(c - *pp));
    } else if (tag_is(tag, "integer")) {
        p = node_new(K93_PLIST_INT);
        p->ival = strtol(*pp, NULL, 10);
    } else if (tag_is(tag, "real")) {
        p = node_new(K93_PLIST_REAL);
        p->rval = strtod(*pp, NULL);
    } else if (tag_is(tag, "true") || tag_is(tag, "false")) {
        p = node_new(K93_PLIST_BOOL);
        p->bval = tag_is(tag, "true");
    } else if (tag_is(tag, "data")) {
        p = node_new(K93_PLIST_DATA);
        p->str = decode_entities(*pp, (size_t)(c - *pp));
    } else if (tag_is(tag, "date")) {
        p = node_new(K93_PLIST_DATE);
        p->str = decode_entities(*pp, (size_t)(c - *pp));
    } else {
        plist_err(eb, el, "unknown plist element", tag);
        return NULL;
    }
    /* verify close tag name matches: "</" + name + ">" */
    if (close_end - c < (long)strlen(tag) + 2 || strncmp(c + 2, tag, strlen(tag))) {
        plist_err(eb, el, "mismatched close tag", c);
        k93_plist_free(p);
        return NULL;
    }
    *pp = close_end + 1;
    return p;
}

k93_plist *k93_plist_parse(const char *xml, size_t len, char *eb, size_t el)
{
    if (eb && el) eb[0] = 0;
    if (len >= 6 && !memcmp(xml, "bplist", 6)) {
        plist_err(eb, el, "binary plist not supported by this parser", xml);
        return NULL;
    }
    const char *p = xml, *end = xml + len;
    for (;;) {
        char tag[64]; const char *attrs = NULL; int closing = 0;
        const char *t = next_tag(&p, end, tag, sizeof tag, &attrs, &closing);
        if (!t) { plist_err(eb, el, "no <plist> root found", xml); return NULL; }
        if (closing) { plist_err(eb, el, "unexpected closing tag at top level", tag); return NULL; }
        if (tag_is(tag, "plist")) break;    /* root found */
        /* skip declarations / doctype */
        if (tag_is(tag, "?xml") || tag_is(tag, "!DOCTYPE")) continue;
        plist_err(eb, el, "unexpected top-level tag", tag);
        return NULL;
    }
    k93_plist *v = parse_value(&p, end, eb, el);
    if (!v) return NULL;
    if (v->type == K93_PLIST_DICT && v->nitems == 2 && v->items[0]->str && !v->items[0]->str[0]) {
        /* some tools wrap: {} root dict - keep as is */
    }
    return v;
}

k93_plist *k93_plist_parse_file(const char *path, char *eb, size_t el)
{
    size_t len = 0;
    char *xml = k93_read_file(path, &len);
    if (!xml) { plist_err(eb, el, "cannot read file", path); return NULL; }
    k93_plist *p = k93_plist_parse(xml, len, eb, el);
    free(xml);
    return p;
}

k93_plist_type k93_plist_type_of(const k93_plist *p) { return p ? p->type : K93_PLIST_STRING; }

const k93_plist *k93_plist_dict_get(const k93_plist *dict, const char *key)
{
    if (!dict || dict->type != K93_PLIST_DICT) return NULL;
    for (int i = 0; i + 1 < dict->nitems; i += 2)
        if (dict->items[i]->str && !strcmp(dict->items[i]->str, key))
            return dict->items[i + 1];
    return NULL;
}

const char *k93_plist_dict_str(const k93_plist *dict, const char *key, const char *dflt)
{
    const k93_plist *v = k93_plist_dict_get(dict, key);
    const char *s = v ? k93_plist_string(v) : NULL;
    return s ? s : dflt;
}

long k93_plist_dict_int(const k93_plist *dict, const char *key, long dflt)
{
    const k93_plist *v = k93_plist_dict_get(dict, key);
    return v ? k93_plist_int(v, dflt) : dflt;
}

int k93_plist_array_count(const k93_plist *arr)
{
    return (arr && arr->type == K93_PLIST_ARRAY) ? arr->nitems : 0;
}

const k93_plist *k93_plist_array_at(const k93_plist *arr, int idx)
{
    if (!arr || arr->type != K93_PLIST_ARRAY || idx < 0 || idx >= arr->nitems) return NULL;
    return arr->items[idx];
}

const char *k93_plist_string(const k93_plist *p)
{
    return (p && p->type == K93_PLIST_STRING) ? p->str : NULL;
}

const char *k93_plist_data(const k93_plist *p, size_t *len)
{
    if (!p || p->type != K93_PLIST_DATA || !p->str) return NULL;
    if (len) *len = strlen(p->str);
    return p->str;
}

long k93_plist_int(const k93_plist *p, long dflt)
{
    if (!p) return dflt;
    if (p->type == K93_PLIST_INT) return p->ival;
    if (p->type == K93_PLIST_REAL) return (long)p->rval;
    if (p->type == K93_PLIST_BOOL) return p->bval;
    if (p->type == K93_PLIST_STRING && p->str) return strtol(p->str, NULL, 10);
    return dflt;
}

int k93_plist_bool(const k93_plist *p, int dflt)
{
    if (!p) return dflt;
    if (p->type == K93_PLIST_BOOL) return p->bval;
    return (int)k93_plist_int(p, dflt);
}

/* ---------- serialization (small: enough to write our own registry plists) ---------- */

static void indent_f(FILE *f, int n) { for (int i = 0; i < n; i++) fputc('\t', f); }

int k93_plist_write_string(FILE *f, const char *s)
{
    fprintf(f, "<string>");
    for (const char *p = s ? s : ""; *p; p++) {
        switch (*p) {
        case '&': fputs("&amp;", f); break;
        case '<': fputs("&lt;", f); break;
        case '>': fputs("&gt;", f); break;
        default: fputc(*p, f);
        }
    }
    fputs("</string>\n", f);
    return 0;
}

int k93_plist_write_integer(FILE *f, long v) { return fprintf(f, "<integer>%ld</integer>\n", v) < 0 ? -1 : 0; }
int k93_plist_write_bool(FILE *f, int v) { return fprintf(f, v ? "<true/>\n" : "<false/>\n") < 0 ? -1 : 0; }

int k93_plist_write_key(FILE *f, const char *k)
{
    fputs("<key>", f);
    for (const char *p = k; *p; p++) {
        switch (*p) {
        case '&': fputs("&amp;", f); break;
        case '<': fputs("&lt;", f); break;
        case '>': fputs("&gt;", f); break;
        default: fputc(*p, f);
        }
    }
    fputs("</key>\n", f);
    return 0;
}

int k93_plist_write_dict_header(FILE *f, int indent)
{
    indent_f(f, indent);
    fputs("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n", f);
    indent_f(f, indent);
    fputs("<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n", f);
    indent_f(f, indent);
    fputs("<plist version=\"1.0\">\n", f);
    indent_f(f, indent);
    fputs("<dict>\n", f);
    return 0;
}

static void dump_rec(const k93_plist *p, char *buf, size_t cap, size_t *o, int depth)
{
#define APP(s) do { size_t l = strlen(s); if (*o + l < cap) { memcpy(buf + *o, s, l); *o += l; } } while (0)
    if (!p) { APP("(null)\n"); return; }
    for (int i = 0; i < depth; i++) APP("  ");
    switch (p->type) {
    case K93_PLIST_DICT: {
        APP("dict {\n");
        for (int i = 0; i + 1 < p->nitems; i += 2) {
            for (int j = 0; j < depth + 1; j++) APP("  ");
            char tmp[512];
            snprintf(tmp, sizeof tmp, "%s = ", p->items[i]->str ? p->items[i]->str : "?");
            APP(tmp);
            dump_rec(p->items[i + 1], buf, cap, o, depth + 1);
        }
        for (int i = 0; i < depth; i++) APP("  ");
        APP("}\n");
        break;
    }
    case K93_PLIST_ARRAY: {
        APP("array [\n");
        for (int i = 0; i < p->nitems; i++) dump_rec(p->items[i], buf, cap, o, depth + 1);
        for (int i = 0; i < depth; i++) APP("  ");
        APP("]\n");
        break;
    }
    case K93_PLIST_STRING: { char tmp[600]; snprintf(tmp, sizeof tmp, "\"%s\"\n", p->str ? p->str : ""); APP(tmp); break; }
    case K93_PLIST_INT:    { char tmp[64]; snprintf(tmp, sizeof tmp, "%ld\n", p->ival); APP(tmp); break; }
    case K93_PLIST_REAL:   { char tmp[64]; snprintf(tmp, sizeof tmp, "%g\n", p->rval); APP(tmp); break; }
    case K93_PLIST_BOOL:   APP(p->bval ? "true\n" : "false\n"); break;
    case K93_PLIST_DATA:   APP("(data)\n"); break;
    case K93_PLIST_DATE:   { char tmp[128]; snprintf(tmp, sizeof tmp, "date %s\n", p->str ? p->str : ""); APP(tmp); break; }
    }
#undef APP
}

char *k93_plist_dump(const k93_plist *p, int *len)
{
    size_t cap = 1 << 16;
    char *buf = k93_xmalloc(cap);
    size_t o = 0;
    dump_rec(p, buf, cap, &o, 0);
    buf[o] = 0;
    if (len) *len = (int)o;
    return buf;
}
