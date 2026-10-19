/* js.c - QuickJS engine for Safari. One JS "Node" wrapper per DOM node
 * (cached so script identity works), DOM methods bound as C functions on a
 * prototype, document/location/fetch/XHR/timers/console in a JS bootstrap.
 * Listeners live JS-side (Map on wrapper identity). */
#define _GNU_SOURCE
#include "safari.h"
#include "k93util.h"
#include "quickjs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static JSRuntime *rt;
static JSContext *ctx;
static sf_page *PG;
static k93node *DOC;
static int dirty;
static int scripts_done;
static char navurl[1280];
static char cookiejar[1024];

/* ---------------------------------------------------------- wrapper cache */

#define MAXWRAP 4096
static struct { k93node *n; JSValue v; } wrap[MAXWRAP];
static int nwrap;

static JSClassID node_class;

static JSValue node_wrap(JSContext *c, k93node *n)
{
    if (!n) return JS_NULL;
    for (int i = 0; i < nwrap; i++)
        if (wrap[i].n == n) return JS_DupValue(c, wrap[i].v);
    JSValue v = JS_NewObjectClass(c, node_class);
    if (JS_IsException(v)) return JS_NULL;
    JS_SetOpaque(v, n);
    if (nwrap < MAXWRAP) {
        wrap[nwrap].n = n;
        wrap[nwrap].v = JS_DupValue(c, v);
        nwrap++;
    }
    return v;
}

static k93node *node_of(JSContext *c, JSValueConst v)
{
    return JS_GetOpaque2(c, v, node_class);
}

/* ------------------------------------------------------------ timers */

#define MAXTIMERS 32
static struct { int active; JSValue fn; int64_t due, interval; } T[MAXTIMERS];

static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static JSValue g_set_timeout(JSContext *c, JSValueConst thisv, int argc,
                             JSValueConst *argv, int interval)
{
    (void)thisv;
    int64_t ms = 0;
    if (argc > 1) JS_ToInt64(c, &ms, argv[1]);
    if (ms < 0) ms = 0;
    if (ms > 600000) ms = 600000;
    for (int i = 0; i < MAXTIMERS; i++) {
        if (T[i].active) continue;
        T[i].active = 1;
        T[i].fn = JS_DupValue(c, argv[0]);
        T[i].due = now_ms() + ms;
        T[i].interval = interval ? (ms > 0 ? ms : 1) : 0;
        return JS_NewInt32(c, i + 1);
    }
    return JS_NewInt32(c, 0);
}

static JSValue c_setTimeout(JSContext *c, JSValueConst thisv, int argc,
                            JSValueConst *argv)
{
    return g_set_timeout(c, thisv, argc, argv, 0);
}

static JSValue c_setInterval(JSContext *c, JSValueConst thisv, int argc,
                             JSValueConst *argv)
{
    return g_set_timeout(c, thisv, argc, argv, 1);
}

static void timer_clear(JSContext *c, int id)
{
    if (id < 1 || id > MAXTIMERS || !T[id - 1].active) return;
    JS_FreeValue(c, T[id - 1].fn);
    T[id - 1].fn = JS_UNDEFINED;
    T[id - 1].active = 0;
}

static JSValue c_clearTimer(JSContext *c, JSValueConst thisv, int argc,
                            JSValueConst *argv)
{
    (void)thisv;
    int id = 0;
    if (argc > 0) JS_ToInt32(c, &id, argv[0]);
    timer_clear(c, id);
    return JS_UNDEFINED;
}

static void drain_jobs(void)
{
    if (!rt) return;
    JSContext *jc;
    while (JS_ExecutePendingJob(rt, &jc) > 0) { }
}

static void fire_timers(void)
{
    int64_t now = now_ms();
    for (int i = 0; i < MAXTIMERS; i++) {
        if (!T[i].active || T[i].due > now) continue;
        JSValue r = JS_Call(ctx, T[i].fn, JS_UNDEFINED, 0, NULL);
        if (JS_IsException(r)) {
            JSValue e = JS_GetException(ctx);
            const char *m = JS_ToCString(ctx, e);
            k93_log("safari-js", "timer error: %s", m ? m : "?");
            JS_FreeCString(ctx, m);
            JS_FreeValue(ctx, e);
        } else {
            JS_FreeValue(ctx, r);
        }
        if (T[i].interval) T[i].due = now_ms() + T[i].interval;
        else timer_clear(ctx, i + 1);
    }
}

/* ------------------------------------------------------------- C funcs */

/* bytes -> JS string; invalid UTF-8 bytes become '?' */
static JSValue js_str_sanitized(JSContext *c, const char *s, size_t len)
{
    char *buf = malloc(len * 2 + 1);
    if (!buf) return JS_NewString(c, "");
    size_t o = 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)s[i];
        if (ch < 0x80) { buf[o++] = (char)ch; continue; }
        int need = ch >= 0xF0 ? 3 : ch >= 0xE0 ? 2 : ch >= 0xC0 ? 1 : 0;
        int ok = 1;
        for (int k = 1; k <= need && ok; k++)
            if (i + (size_t)k >= len ||
                ((unsigned char)s[i + k] & 0xC0) != 0x80) ok = 0;
        if (ok) {
            for (int k = 0; k <= need; k++) buf[o++] = s[i + (size_t)k];
            i += (size_t)need;
        } else {
            buf[o++] = '?';
        }
    }
    buf[o] = 0;
    JSValue v = JS_NewStringLen(c, buf, o);
    free(buf);
    return v;
}

static JSValue c_fetch(JSContext *c, JSValueConst thisv, int argc,
                       JSValueConst *argv)
{
    (void)thisv;
    if (argc < 1) return JS_NewString(c, "");
    const char *u = JS_ToCString(ctx, argv[0]);
    if (!u) return JS_NewString(c, "");
    int status = 0;
    char err[64];
    char *body = sf_fetch(u, &status, err, sizeof err);
    JS_FreeCString(ctx, u);
    if (!body) return JS_NewString(c, "");
    JSValue v = js_str_sanitized(c, body, sf_fetch_len());
    free(body);
    return v;
}

/* [status, text] for fetch()/XHR */
static JSValue c_fetch2(JSContext *c, JSValueConst thisv, int argc,
                        JSValueConst *argv)
{
    (void)thisv;
    JSValue arr = JS_NewArray(ctx);
    if (argc < 1) {
        JS_SetPropertyUint32(ctx, arr, 0, JS_NewInt32(ctx, 0));
        JS_SetPropertyUint32(ctx, arr, 1, JS_NewString(c, ""));
        return arr;
    }
    const char *u = JS_ToCString(ctx, argv[0]);
    int status = 0;
    char err[64];
    char *body = u ? sf_fetch(u, &status, err, sizeof err) : NULL;
    if (u) JS_FreeCString(ctx, u);
    JS_SetPropertyUint32(ctx, arr, 0, JS_NewInt32(ctx, status));
    if (body) {
        JS_SetPropertyUint32(ctx, arr, 1,
                             js_str_sanitized(c, body, sf_fetch_len()));
        free(body);
    } else {
        JS_SetPropertyUint32(ctx, arr, 1, JS_NewString(c, ""));
    }
    return arr;
}

static JSValue c_log(JSContext *c, JSValueConst thisv, int argc,
                     JSValueConst *argv)
{
    (void)thisv;
    char line[512];
    size_t o = 0;
    line[0] = 0;
    for (int i = 0; i < argc; i++) {
        const char *s = JS_ToCString(ctx, argv[i]);
        if (!s) continue;
        size_t sl = strlen(s);
        if (o + sl + 2 >= sizeof line) sl = sizeof line - o - 2;
        memcpy(line + o, s, sl);
        o += sl;
        line[o] = 0;
        JS_FreeCString(ctx, s);
        if (i + 1 < argc && o + 1 < sizeof line) { line[o++] = ' '; line[o] = 0; }
    }
    k93_log("safari-js", "%s", line);
    return JS_UNDEFINED;
}

static JSValue c_base(JSContext *c, JSValueConst thisv, int argc,
                      JSValueConst *argv)
{
    (void)thisv; (void)argc; (void)argv;
    return JS_NewString(c, PG ? PG->base : "");
}

static JSValue c_nav(JSContext *c, JSValueConst thisv, int argc,
                     JSValueConst *argv)
{
    (void)thisv;
    if (argc < 1) return JS_UNDEFINED;
    const char *u = JS_ToCString(ctx, argv[0]);
    if (u) {
        k93_strlcpy(navurl, u, sizeof navurl);
        JS_FreeCString(ctx, u);
    }
    return JS_UNDEFINED;
}

static JSValue c_eval(JSContext *c, JSValueConst thisv, int argc,
                      JSValueConst *argv)
{
    (void)thisv;
    if (argc < 1) return JS_UNDEFINED;
    size_t len = 0;
    const char *code = JS_ToCStringLen(ctx, &len, argv[0]);
    if (!code) return JS_UNDEFINED;
    JSValue r = JS_Eval(ctx, code, len, "eval", JS_EVAL_TYPE_GLOBAL);
    JS_FreeCString(ctx, code);
    if (JS_IsException(r)) {
        JSValue e = JS_GetException(ctx);
        const char *m = JS_ToCString(ctx, e);
        k93_log("safari-js", "eval error: %s", m ? m : "?");
        JS_FreeCString(ctx, m);
        JS_FreeValue(ctx, e);
        return JS_UNDEFINED;
    }
    return r;
}

static JSValue c_alert(JSContext *c, JSValueConst thisv, int argc,
                       JSValueConst *argv)
{
    (void)thisv;
    const char *m = argc > 0 ? JS_ToCString(ctx, argv[0]) : "alert";
    ui_msgbox("JavaScript", m ? m : "alert");
    if (m) JS_FreeCString(ctx, m);
    dirty = 1;
    return JS_UNDEFINED;
}

static JSValue c_readyState(JSContext *c, JSValueConst thisv, int argc,
                            JSValueConst *argv)
{
    (void)thisv; (void)argc; (void)argv;
    return JS_NewString(c, scripts_done ? "complete" : "loading");
}

static JSValue c_getElementById(JSContext *c, JSValueConst thisv, int argc,
                                JSValueConst *argv)
{
    (void)thisv;
    if (argc < 1) return JS_NULL;
    const char *id = JS_ToCString(ctx, argv[0]);
    k93node *n = id ? k93html_get_id(DOC, id) : NULL;
    if (id) JS_FreeCString(ctx, id);
    return node_wrap(c, n);
}

static JSValue c_query(JSContext *c, JSValueConst thisv, int argc,
                       JSValueConst *argv, int all)
{
    (void)thisv;
    if (argc < 1) return all ? JS_NewArray(ctx) : JS_NULL;
    const char *sel = JS_ToCString(ctx, argv[0]);
    if (!sel) return all ? JS_NewArray(ctx) : JS_NULL;
    k93node *out[512];
    int n = k93html_query_all(DOC, sel, out, 512);
    JS_FreeCString(ctx, sel);
    if (!all) return node_wrap(c, n ? out[0] : NULL);
    JSValue arr = JS_NewArray(ctx);
    for (int i = 0; i < n; i++)
        JS_SetPropertyUint32(ctx, arr, (uint32_t)i, node_wrap(c, out[i]));
    return arr;
}

static JSValue c_query1(JSContext *c, JSValueConst thisv, int argc,
                        JSValueConst *argv)
{
    return c_query(c, thisv, argc, argv, 0);
}

static JSValue c_queryAll(JSContext *c, JSValueConst thisv, int argc,
                          JSValueConst *argv)
{
    return c_query(c, thisv, argc, argv, 1);
}

static JSValue c_createElement(JSContext *c, JSValueConst thisv, int argc,
                               JSValueConst *argv)
{
    (void)thisv;
    if (argc < 1) return JS_NULL;
    const char *tag = JS_ToCString(ctx, argv[0]);
    k93node *n = tag ? k93html_elem(tag) : NULL;
    if (tag) JS_FreeCString(ctx, tag);
    return node_wrap(c, n);
}

static JSValue c_createTextNode(JSContext *c, JSValueConst thisv, int argc,
                                JSValueConst *argv)
{
    (void)thisv;
    const char *s = argc > 0 ? JS_ToCString(ctx, argv[0]) : "";
    k93node *n = k93html_textnode(s ? s : "");
    if (s && argc > 0) JS_FreeCString(ctx, s);
    return node_wrap(c, n);
}

static JSValue c_write(JSContext *c, JSValueConst thisv, int argc,
                       JSValueConst *argv)
{
    (void)thisv;
    if (argc < 1 || !DOC) return JS_UNDEFINED;
    const char *s = JS_ToCString(ctx, argv[0]);
    if (!s) return JS_UNDEFINED;
    k93node *b = sf_body(DOC);
    k93node *frag = k93html_parse(s);
    JS_FreeCString(ctx, s);
    if (frag) {
        k93node *ch = frag->first;
        while (ch) {
            k93node *nx = ch->next;
            k93html_append(b, ch);
            ch = nx;
        }
        free(frag->tag);
        free(frag);
        dirty = 1;
    }
    return JS_UNDEFINED;
}

static JSValue c_getTitle(JSContext *c, JSValueConst thisv, int argc,
                          JSValueConst *argv)
{
    (void)thisv; (void)argc; (void)argv;
    char buf[256];
    return JS_NewString(c, sf_title(DOC, buf, sizeof buf));
}

static JSValue c_setTitle(JSContext *c, JSValueConst thisv, int argc,
                          JSValueConst *argv)
{
    (void)thisv;
    if (argc < 1 || !DOC) return JS_UNDEFINED;
    const char *s = JS_ToCString(ctx, argv[0]);
    k93node *t = k93html_query(DOC, "title");
    if (!t) {
        t = k93html_elem("title");
        k93node *h = k93html_query(DOC, "head");
        if (h) k93html_append(h, t);
        else k93html_append(sf_body(DOC), t);
    }
    if (t) {
        /* title is a rawtext element: content may live in t->text */
        free(t->text);
        t->text = NULL;
        k93node *c1 = t->first;
        while (c1) { k93node *nx = c1->next; k93html_free(c1); c1 = nx; }
        t->first = t->last = NULL;
        k93html_append(t, k93html_textnode(s ? s : ""));
    }
    if (s) JS_FreeCString(ctx, s);
    dirty = 1;
    return JS_UNDEFINED;
}

static JSValue c_cookie_get(JSContext *c, JSValueConst thisv, int argc,
                            JSValueConst *argv)
{
    (void)thisv; (void)argc; (void)argv;
    return JS_NewString(c, cookiejar);
}

static JSValue c_cookie_set(JSContext *c, JSValueConst thisv, int argc,
                            JSValueConst *argv)
{
    (void)thisv;
    if (argc < 1) return JS_UNDEFINED;
    const char *s = JS_ToCString(ctx, argv[0]);
    if (s) {
        size_t have = strlen(cookiejar);
        size_t add = strcspn(s, ";");
        if (have + add + 2 < sizeof cookiejar) {
            if (have) cookiejar[have] = ' ';
            memcpy(cookiejar + have + (have ? 1 : 0), s, add);
            cookiejar[have + (have ? 1 : 0) + add] = 0;
        }
        JS_FreeCString(ctx, s);
    }
    return JS_UNDEFINED;
}

/* ---------------------------------------------------- node proto funcs */

enum {
    P_TAGNAME, P_NODENAME, P_NODETYPE, P_ID, P_CLASSNAME, P_TEXTCONTENT,
    P_INNERHTML, P_OUTERHTML, P_PARENTNODE, P_FIRSTCHILD, P_NEXTSIBLING,
    P_PREVSIBLING, P_CHILDREN, P_STYLE
};

/* serialize node + subtree to markup (attrs re-escaped for round-trip) */
static char *serialize_outer(const k93node *n)
{
    if (!n) return NULL;
    if (!strcmp(n->tag, "#text")) {
        char *t = malloc(strlen(n->text ? n->text : "") + 1);
        if (t) strcpy(t, n->text ? n->text : "");
        return t;
    }
    size_t need = 64;
    for (int i = 0; i < n->nattr; i++)
        need += strlen(n->ak[i]) + 2 * (n->av[i] ? strlen(n->av[i]) : 0) + 8;
    char *inner = k93html_inner_html((k93node *)n);
    if (inner) need += strlen(inner);
    char *s = malloc(need);
    if (!s) { free(inner); return NULL; }
    size_t o = (size_t)snprintf(s, need, "<%s", n->tag);
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
    o += (size_t)snprintf(s + o, need - o, ">%s</%s>",
                          inner ? inner : "", n->tag);
    free(inner);
    s[o] = 0;
    return s;
}

static JSValue n_prop_get(JSContext *c, JSValueConst thisv, int magic)
{
    k93node *n = node_of(c, thisv);
    if (!n) return JS_NULL;
    switch (magic) {
    case P_TAGNAME:
        return JS_NewString(c, n->tag);
    case P_NODENAME:
        return JS_NewString(c, n->tag);
    case P_NODETYPE:
        return JS_NewInt32(c, strcmp(n->tag, "#text") ? 1 : 3);
    case P_ID: {
        const char *v = k93html_attr(n, "id");
        return JS_NewString(c, v ? v : "");
    }
    case P_CLASSNAME: {
        const char *v = k93html_attr(n, "class");
        return JS_NewString(c, v ? v : "");
    }
    case P_TEXTCONTENT: {
        char buf[8192];
        k93html_text_content(n, buf, sizeof buf);
        return JS_NewString(c, buf);
    }
    case P_INNERHTML: {
        char *s = k93html_inner_html(n);
        JSValue v = JS_NewString(c, s ? s : "");
        free(s);
        return v;
    }
    case P_OUTERHTML: {
        char *s = serialize_outer(n);
        JSValue v = JS_NewString(c, s ? s : "");
        free(s);
        return v;
    }
    case P_PARENTNODE:
        return node_wrap(c, n->parent && strcmp(n->parent->tag, "document")
                                ? n->parent : NULL);
    case P_FIRSTCHILD:
        return node_wrap(c, n->first);
    case P_NEXTSIBLING:
        return node_wrap(c, n->next);
    case P_PREVSIBLING:
        return node_wrap(c, n->prev);
    case P_CHILDREN: {
        JSValue arr = JS_NewArray(ctx);
        uint32_t i = 0;
        for (k93node *k = n->first; k; k = k->next)
            if (strcmp(k->tag, "#text"))
                JS_SetPropertyUint32(ctx, arr, i++, node_wrap(c, k));
        return arr;
    }
    default:
        return JS_NULL;
    }
}

static JSValue n_prop_set(JSContext *c, JSValueConst thisv, JSValueConst val,
                          int magic)
{
    k93node *n = node_of(c, thisv);
    if (!n) return JS_UNDEFINED;
    const char *s = JS_ToCString(ctx, val);
    if (!s) return JS_UNDEFINED;
    if (magic == P_TEXTCONTENT) {
        k93node *k = n->first;
        while (k) { k93node *nx = k->next; k93html_free(k); k = nx; }
        n->first = n->last = NULL;
        free(n->text);
        n->text = NULL;
        if (s[0]) k93html_append(n, k93html_textnode(s));
        dirty = 1;
    } else if (magic == P_INNERHTML) {
        k93html_set_inner_html(n, s);
        dirty = 1;
    } else if (magic == P_OUTERHTML && n->parent) {
        /* replace the node itself with the parsed fragment; the old node is
         * unlinked but kept alive (js wrappers may still hold it) */
        k93node *par = n->parent;
        k93node *anchor = n->next;
        k93node *frag = k93html_parse(s);
        k93node *cur = frag ? frag->first : NULL;
        k93html_unlink(n);
        while (cur) {
            k93node *nx = cur->next;
            if (anchor) k93html_insert_before(anchor, cur);
            else k93html_append(par, cur);
            cur = nx;
        }
        k93html_free(frag);
        dirty = 1;
    }
    JS_FreeCString(ctx, s);
    return JS_UNDEFINED;
}

static JSValue n_getAttribute(JSContext *c, JSValueConst thisv, int argc,
                              JSValueConst *argv)
{
    k93node *n = node_of(c, thisv);
    if (!n || argc < 1) return JS_UNDEFINED;
    const char *k = JS_ToCString(ctx, argv[0]);
    const char *v = k ? k93html_attr(n, k) : NULL;
    JSValue r = v ? JS_NewString(c, v) : JS_UNDEFINED;
    if (k) JS_FreeCString(ctx, k);
    return r;
}

static JSValue n_setAttribute(JSContext *c, JSValueConst thisv, int argc,
                              JSValueConst *argv)
{
    k93node *n = node_of(c, thisv);
    if (!n || argc < 2) return JS_UNDEFINED;
    const char *k = JS_ToCString(ctx, argv[0]);
    const char *v = JS_ToCString(ctx, argv[1]);
    if (k) k93html_set_attr(n, k, v ? v : "");
    if (k) JS_FreeCString(ctx, k);
    if (v) JS_FreeCString(ctx, v);
    dirty = 1;
    return JS_UNDEFINED;
}

static JSValue n_removeAttribute(JSContext *c, JSValueConst thisv, int argc,
                                 JSValueConst *argv)
{
    k93node *n = node_of(c, thisv);
    if (!n || argc < 1) return JS_UNDEFINED;
    const char *k = JS_ToCString(ctx, argv[0]);
    if (k) {
        for (int i = 0; i < n->nattr; i++)
            if (!strcmp(n->ak[i], k)) {
                free(n->ak[i]);
                free(n->av[i]);
                n->ak[i] = n->ak[n->nattr - 1];
                n->av[i] = n->av[n->nattr - 1];
                n->nattr--;
                break;
            }
        JS_FreeCString(ctx, k);
    }
    dirty = 1;
    return JS_UNDEFINED;
}

static k93node *arg_node(JSContext *c, JSValueConst v)
{
    return JS_GetOpaque(v, node_class);
}

static JSValue n_appendChild(JSContext *c, JSValueConst thisv, int argc,
                             JSValueConst *argv)
{
    k93node *n = node_of(c, thisv);
    k93node *ch = argc > 0 ? arg_node(c, argv[0]) : NULL;
    if (!n || !ch) return JS_UNDEFINED;
    k93html_append(n, ch);
    dirty = 1;
    return JS_DupValue(c, argv[0]);
}

static JSValue n_insertBefore(JSContext *c, JSValueConst thisv, int argc,
                              JSValueConst *argv)
{
    k93node *n = node_of(c, thisv);
    k93node *ch = argc > 0 ? arg_node(c, argv[0]) : NULL;
    k93node *ref = argc > 1 ? arg_node(c, argv[1]) : NULL;
    if (!n || !ch) return JS_UNDEFINED;
    if (ref) k93html_insert_before(ref, ch);
    else k93html_append(n, ch);
    dirty = 1;
    return JS_DupValue(c, argv[0]);
}

static JSValue n_removeChild(JSContext *c, JSValueConst thisv, int argc,
                             JSValueConst *argv)
{
    k93node *n = node_of(c, thisv);
    k93node *ch = argc > 0 ? arg_node(c, argv[0]) : NULL;
    if (!n || !ch || ch->parent != n) return JS_UNDEFINED;
    k93html_unlink(ch);
    dirty = 1;
    return JS_DupValue(c, argv[0]);
}

static JSValue n_remove(JSContext *c, JSValueConst thisv, int argc,
                        JSValueConst *argv)
{
    (void)argc; (void)argv;
    k93node *n = node_of(c, thisv);
    if (n && n->parent) { k93html_unlink(n); dirty = 1; }
    return JS_UNDEFINED;
}

static JSValue n_replaceChild(JSContext *c, JSValueConst thisv, int argc,
                              JSValueConst *argv)
{
    k93node *n = node_of(c, thisv);
    k93node *nw = argc > 0 ? arg_node(c, argv[0]) : NULL;
    k93node *od = argc > 1 ? arg_node(c, argv[1]) : NULL;
    if (!n || !nw || !od || od->parent != n) return JS_UNDEFINED;
    k93html_insert_before(od, nw);        /* new before old */
    k93html_unlink(od);                   /* old orphaned, kept alive */
    dirty = 1;
    return node_wrap(c, od);
}

static JSValue n_contains(JSContext *c, JSValueConst thisv, int argc,
                          JSValueConst *argv)
{
    k93node *n = node_of(c, thisv);
    k93node *o = argc > 0 ? arg_node(c, argv[0]) : NULL;
    for (k93node *k = o; k; k = k->parent)
        if (k == n) return JS_TRUE;
    return JS_FALSE;
}

static JSValue n_hasChildNodes(JSContext *c, JSValueConst thisv, int argc,
                               JSValueConst *argv)
{
    (void)argc; (void)argv;
    k93node *n = node_of(c, thisv);
    return JS_NewBool(c, n && n->first != NULL);
}

static JSValue n_cloneNode(JSContext *c, JSValueConst thisv, int argc,
                           JSValueConst *argv)
{
    k93node *n = node_of(c, thisv);
    if (!n) return JS_NULL;
    int deep = argc > 0 ? JS_ToBool(c, argv[0]) : 0;
    k93node *k;
    if (!strcmp(n->tag, "#text"))
        k = k93html_textnode(n->text ? n->text : "");
    else if (deep)
        k = k93html_clone(n);
    else {
        k = k93html_elem(n->tag);
        if (k)
            for (int i = 0; i < n->nattr; i++)
                k93html_set_attr(k, n->ak[i], n->av[i] ? n->av[i] : "");
    }
    return node_wrap(c, k);
}

/* node-local query (scoped to subtree) */
static JSValue n_query(JSContext *c, JSValueConst thisv, int argc,
                       JSValueConst *argv, int all)
{
    k93node *n = node_of(c, thisv);
    if (!n || argc < 1) return all ? JS_NewArray(c) : JS_NULL;
    const char *sel = JS_ToCString(ctx, argv[0]);
    if (!sel) return all ? JS_NewArray(c) : JS_NULL;
    k93node *out[256];
    int cnt = k93html_query_all(n, sel, out, 256);
    JS_FreeCString(ctx, sel);
    if (!all) return node_wrap(c, cnt ? out[0] : NULL);
    JSValue arr = JS_NewArray(c);
    for (int i = 0; i < cnt; i++)
        JS_SetPropertyUint32(c, arr, (uint32_t)i, node_wrap(c, out[i]));
    return arr;
}

static JSValue n_query1(JSContext *c, JSValueConst thisv, int argc,
                        JSValueConst *argv)
{
    return n_query(c, thisv, argc, argv, 0);
}

static JSValue n_queryAll(JSContext *c, JSValueConst thisv, int argc,
                          JSValueConst *argv)
{
    return n_query(c, thisv, argc, argv, 1);
}

static JSValue n_getElementsByTagName(JSContext *c, JSValueConst thisv,
                                      int argc, JSValueConst *argv)
{
    return n_queryAll(c, thisv, argc, argv);
}

/* dispatch click on this node via bootstrap __k93_click */
static int dispatch_click(k93node *n)
{
    if (!ctx || !n) return 0;
    JSValue g = JS_GetGlobalObject(ctx);
    JSValue f = JS_GetPropertyStr(ctx, g, "__k93_click");
    JS_FreeValue(ctx, g);
    int rv = 0;
    if (JS_IsFunction(ctx, f)) {
        JSValue nv = node_wrap(ctx, n);
        JSValue r = JS_Call(ctx, f, JS_UNDEFINED, 1, &nv);
        JS_FreeValue(ctx, nv);
        if (JS_IsNumber(r)) JS_ToInt32(ctx, &rv, r);
        JS_FreeValue(ctx, r);
        JS_FreeValue(ctx, f);
    } else {
        JS_FreeValue(ctx, f);
    }
    drain_jobs();
    return rv;
}

static JSValue n_click(JSContext *c, JSValueConst thisv, int argc,
                       JSValueConst *argv)
{
    (void)argc; (void)argv;
    k93node *n = node_of(c, thisv);
    dispatch_click(n);
    return JS_UNDEFINED;
}

static const JSCFunctionListEntry node_proto_list[] = {
    JS_CFUNC_DEF("getAttribute", 1, n_getAttribute),
    JS_CFUNC_DEF("setAttribute", 2, n_setAttribute),
    JS_CFUNC_DEF("removeAttribute", 1, n_removeAttribute),
    JS_CFUNC_DEF("appendChild", 1, n_appendChild),
    JS_CFUNC_DEF("insertBefore", 2, n_insertBefore),
    JS_CFUNC_DEF("removeChild", 1, n_removeChild),
    JS_CFUNC_DEF("remove", 0, n_remove),
    JS_CFUNC_DEF("replaceChild", 2, n_replaceChild),
    JS_CFUNC_DEF("contains", 1, n_contains),
    JS_CFUNC_DEF("hasChildNodes", 0, n_hasChildNodes),
    JS_CFUNC_DEF("cloneNode", 1, n_cloneNode),
    JS_CFUNC_DEF("querySelector", 1, n_query1),
    JS_CFUNC_DEF("querySelectorAll", 1, n_queryAll),
    JS_CFUNC_DEF("getElementsByTagName", 1, n_getElementsByTagName),
    JS_CFUNC_DEF("click", 0, n_click),
    JS_CGETSET_MAGIC_DEF("tagName", n_prop_get, NULL, P_TAGNAME),
    JS_CGETSET_MAGIC_DEF("nodeName", n_prop_get, NULL, P_NODENAME),
    JS_CGETSET_MAGIC_DEF("nodeType", n_prop_get, NULL, P_NODETYPE),
    JS_CGETSET_MAGIC_DEF("id", n_prop_get, NULL, P_ID),
    JS_CGETSET_MAGIC_DEF("className", n_prop_get, NULL, P_CLASSNAME),
    JS_CGETSET_MAGIC_DEF("textContent", n_prop_get, n_prop_set, P_TEXTCONTENT),
    JS_CGETSET_MAGIC_DEF("innerHTML", n_prop_get, n_prop_set, P_INNERHTML),
    JS_CGETSET_MAGIC_DEF("outerHTML", n_prop_get, n_prop_set, P_OUTERHTML),
    JS_CGETSET_MAGIC_DEF("parentNode", n_prop_get, NULL, P_PARENTNODE),
    JS_CGETSET_MAGIC_DEF("parentElement", n_prop_get, NULL, P_PARENTNODE),
    JS_CGETSET_MAGIC_DEF("firstChild", n_prop_get, NULL, P_FIRSTCHILD),
    JS_CGETSET_MAGIC_DEF("nextSibling", n_prop_get, NULL, P_NEXTSIBLING),
    JS_CGETSET_MAGIC_DEF("previousSibling", n_prop_get, NULL, P_PREVSIBLING),
    JS_CGETSET_MAGIC_DEF("children", n_prop_get, NULL, P_CHILDREN),
    JS_CGETSET_MAGIC_DEF("style", n_prop_get, NULL, P_STYLE),
};

/* ------------------------------------------------- bootstrap (JS side) */

static const char bootstrap_js[] =
"globalThis.console = { log: function () { sf_log([].slice.call(arguments).join(\" \")); },\n"
"  warn: function () { sf_log([].slice.call(arguments).join(\" \")); },\n"
"  error: function () { sf_log([].slice.call(arguments).join(\" \")); },\n"
"  info: function () { sf_log([].slice.call(arguments).join(\" \")); } };\n"
"globalThis.navigator = { userAgent: \"K93Safari/2.0 (K93OS; x86_64)\",\n"
"  language: \"en-US\", platform: \"Linux x86_64\", onLine: true };\n"
"var location = {\n"
"  get href() { return sf_base(); },\n"
"  set href(v) { sf_nav(String(v)); },\n"
"  get protocol() { return sf_base().split(\":\")[0] + \":\"; },\n"
"  get host() { var b = sf_base().replace(/^[a-z]+:\\/\\//, \"\"); return b.split(\"/\")[0]; },\n"
"  get hostname() { return this.host.split(\":\")[0]; },\n"
"  get pathname() { var b = sf_base(); var i = b.indexOf(\"/\", b.indexOf(\"//\") + 2);\n"
"    return i < 0 ? \"/\" : b.slice(i).split(\"?\")[0]; },\n"
"  get search() { var b = sf_base(); var i = b.indexOf(\"?\"); return i < 0 ? \"\" : b.slice(i); },\n"
"  get origin() { return sf_base().replace(/\\/.*$/, \"\"); },\n"
"  reload: function () { sf_nav(sf_base()); },\n"
"  toString: function () { return sf_base(); }\n"
"};\n"
"var __listeners = new Map();\n"
"var P = __sf_proto;\n"
"P.addEventListener = function (t, f) {\n"
"  if (typeof f !== \"function\") return;\n"
"  var a = __listeners.get(this); if (!a) { a = []; __listeners.set(this, a); }\n"
"  for (var i = 0; i < a.length; i++) if (a[i].t === t && a[i].f === f) return;\n"
"  a.push({ t: t, f: f });\n"
"};\n"
"P.removeEventListener = function (t, f) {\n"
"  var a = __listeners.get(this); if (!a) return;\n"
"  for (var i = 0; i < a.length; i++)\n"
"    if (a[i].t === t && a[i].f === f) { a.splice(i, 1); return; }\n"
"};\n"
"P.dispatchEvent = function (ev) { if (ev && ev.type === \"click\") this.click(); return true; };\n"
"P.focus = function () {}; P.blur = function () {};\n"
"P.scrollIntoView = function () {};\n"
"P.getBoundingClientRect = function () { return { left: 0, top: 0, right: 320, bottom: 480,\n"
"  width: 320, height: 480, x: 0, y: 0 }; };\n"
"P.getElementsByTagName = P.getElementsByTagName || function (t) { return this.querySelectorAll(t); };\n"
"Object.defineProperty(P, \"style\", { get: function () {\n"
"  var self = this;\n"
"  var obj = {\n"
"    setProperty: function (k, v) {\n"
"      var s = self.getAttribute(\"style\") || \"\";\n"
"      var re = new RegExp(\"(?:^|;)\\\\s*\" + k.replace(/-/g, \"\\\\-\") + \"\\\\s*:[^;]*\", \"i\");\n"
"      s = s.replace(re, \"\").replace(/^;\\s*/, \"\");\n"
"      self.setAttribute(\"style\", s + \"; \" + k + \": \" + v);\n"
"    },\n"
"    getPropertyValue: function (k) {\n"
"      var s = self.getAttribute(\"style\") || \"\";\n"
"      var m = s.match(new RegExp(\"(?:^|;)\\\\s*\" + k.replace(/-/g, \"\\\\-\") + \"\\\\s*:\\\\s*([^;]*)\", \"i\"));\n"
"      return m ? m[1].trim() : \"\";\n"
"    },\n"
"    removeProperty: function (k) { this.setProperty(k, \"\"); return \"\"; }\n"
"  };\n"
"  return new Proxy(obj, {\n"
"    get: function (t, k) {\n"
"      if (typeof k !== \"string\" || k in t) return t[k];\n"
"      return t.getPropertyValue(k.replace(/[A-Z]/g,\n"
"        function (m) { return \"-\" + m.toLowerCase(); }));\n"
"    },\n"
"    set: function (t, k, v) {\n"
"      if (typeof k === \"string\")\n"
"        t.setProperty(k.replace(/[A-Z]/g,\n"
"          function (m) { return \"-\" + m.toLowerCase(); }), String(v));\n"
"      return true;\n"
"    }\n"
"  });\n"
"}});\n"
"function __cls(el) {\n"
"  return (el.getAttribute(\"class\") || \"\").trim().split(/\\s+/).filter(Boolean);\n"
"}\n"
"Object.defineProperty(P, \"classList\", { get: function () {\n"
"  var el = this;\n"
"  return {\n"
"    get length() { return __cls(el).length; },\n"
"    item: function (i) { return __cls(el)[i] || null; },\n"
"    contains: function (c) { return __cls(el).indexOf(c) >= 0; },\n"
"    add: function () {\n"
"      var cs = __cls(el);\n"
"      for (var i = 0; i < arguments.length; i++)\n"
"        if (cs.indexOf(arguments[i]) < 0) cs.push(arguments[i]);\n"
"      el.setAttribute(\"class\", cs.join(\" \"));\n"
"    },\n"
"    remove: function () {\n"
"      var cs = __cls(el);\n"
"      for (var i = 0; i < arguments.length; i++) {\n"
"        var k = cs.indexOf(arguments[i]);\n"
"        if (k >= 0) cs.splice(k, 1);\n"
"      }\n"
"      el.setAttribute(\"class\", cs.join(\" \"));\n"
"    },\n"
"    toggle: function (c) {\n"
"      var cs = __cls(el); var k = cs.indexOf(c);\n"
"      if (k >= 0) cs.splice(k, 1); else cs.push(c);\n"
"      el.setAttribute(\"class\", cs.join(\" \"));\n"
"      return k < 0;\n"
"    }\n"
"  };\n"
"}});\n"
"function __dskey(k) {\n"
"  return \"data-\" + k.replace(/[A-Z]/g,\n"
"    function (m) { return \"-\" + m.toLowerCase(); });\n"
"}\n"
"Object.defineProperty(P, \"dataset\", { get: function () {\n"
"  var el = this;\n"
"  /* ceiling: no ownKeys trap - for..in over dataset unsupported */\n"
"  return new Proxy({}, {\n"
"    get: function (t, k) {\n"
"      return (typeof k === \"string\") ? el.getAttribute(__dskey(k)) : undefined;\n"
"    },\n"
"    set: function (t, k, v) {\n"
"      if (typeof k === \"string\") el.setAttribute(__dskey(k), String(v));\n"
"      return true;\n"
"    },\n"
"    has: function (t, k) {\n"
"      return typeof k === \"string\" && el.getAttribute(__dskey(k)) !== null;\n"
"    }\n"
"  });\n"
"}});\n"
"globalThis.__k93_click = function (node) {\n"
"  var ls = __listeners.get(node) || [];\n"
"  var ev = { type: \"click\", target: node, currentTarget: node,\n"
"    defaultPrevented: false, preventDefault: function () { ev.defaultPrevented = true; },\n"
"    stopPropagation: function () {} };\n"
"  var handled = ls.length > 0;\n"
"  for (var i = 0; i < ls.length; i++) {\n"
"    try { ls[i].f.call(node, ev); } catch (e) { console.log(\"click: \" + e); }\n"
"  }\n"
"  var oc = null;\n"
"  try { oc = node.getAttribute && node.getAttribute(\"onclick\"); } catch (e) {}\n"
"  if (oc) { handled = true;\n"
"    try { (new Function(\"event\", oc)).call(node, ev); } catch (e) { console.log(\"onclick: \" + e); }\n"
"  }\n"
"  return ev.defaultPrevented ? 2 : (handled ? 1 : 0);\n"
"};\n"
"function XMLHttpRequest() { this.readyState = 0; this.status = 0;\n"
"  this.responseText = \"\"; this.onreadystatechange = null; this._u = \"\"; }\n"
"XMLHttpRequest.prototype.open = function (m, u) { this._u = String(u); this.readyState = 1; };\n"
"XMLHttpRequest.prototype.send = function () {\n"
"  var a = sf_xhr(this._u);\n"
"  this.status = a[0]; this.responseText = a[1]; this.readyState = 4;\n"
"  if (this.onreadystatechange) this.onreadystatechange();\n"
"};\n"
"XMLHttpRequest.prototype.setRequestHeader = function () {};\n"
"XMLHttpRequest.prototype.getAllResponseHeaders = function () { return \"\"; };\n"
"globalThis.XMLHttpRequest = XMLHttpRequest;\n"
"function __resp(u) { var a = sf_xhr(u); return {\n"
"  ok: a[0] >= 200 && a[0] < 300, status: a[0], url: u,\n"
"  text: function () { return Promise.resolve(a[1]); },\n"
"  json: function () { try { return Promise.resolve(JSON.parse(a[1])); }\n"
"    catch (e) { return Promise.reject(e); } },\n"
"  headers: { get: function () { return \"\"; } } }; }\n"
"globalThis.fetch = function (u) { return Promise.resolve(__resp(String(u))); };\n"
"globalThis.setTimeout = function (f, ms) { return sf_setTimeout(f, ms || 0); };\n"
"globalThis.clearTimeout = function (id) { sf_clearTimeout(id); };\n"
"globalThis.setInterval = function (f, ms) { return sf_setInterval(f, ms || 0); };\n"
"globalThis.clearInterval = function (id) { sf_clearTimeout(id); };\n"
"globalThis.alert = function (m) { sf_alert(String(m)); };\n"
"var __doclsn = [];\n"
"var document = {\n"
"  get body() { return sf_query(\"body\"); },\n"
"  get documentElement() { return sf_query(\"html\"); },\n"
"  get head() { return sf_query(\"head\"); },\n"
"  getElementById: function (i) { return sf_getElementById(i); },\n"
"  querySelector: function (s) { return sf_query(s); },\n"
"  querySelectorAll: function (s) { return sf_queryAll(s); },\n"
"  getElementsByTagName: function (t) { return sf_queryAll(t); },\n"
"  getElementsByClassName: function (t) { return sf_queryAll(\".\" + t); },\n"
"  createElement: function (t) { return sf_createElement(t); },\n"
"  createTextNode: function (t) { return sf_createTextNode(t); },\n"
"  write: function (h) { sf_write(h); },\n"
"  writeln: function (h) { sf_write(h + \"\\n\"); },\n"
"  open: function () {}, close: function () {},\n"
"  get title() { return sf_getTitle(); },\n"
"  set title(v) { sf_setTitle(String(v)); },\n"
"  get cookie() { return sf_getCookie(); },\n"
"  set cookie(v) { sf_setCookie(String(v)); },\n"
"  addEventListener: function (t, f) {\n"
"    if (typeof f === \"function\") __doclsn.push({ t: t, f: f });\n"
"    var b = document.body; if (b) b.addEventListener(t, f);\n"
"  },\n"
"  removeEventListener: function (t, f) {\n"
"    for (var i = 0; i < __doclsn.length; i++)\n"
"      if (__doclsn[i].t === t && __doclsn[i].f === f) { __doclsn.splice(i, 1); break; }\n"
"    var b = document.body; if (b) b.removeEventListener(t, f);\n"
"  },\n"
"  get readyState() { return sf_readyState(); },\n"
"  get currentScript() { return null; },\n"
"  documentMode: 0\n"
"};\n"
"globalThis.window = globalThis;\n"
"globalThis.document = document;\n"
"globalThis.location = location;\n"
"globalThis.self = globalThis;\n"
"globalThis.top = globalThis;\n"
"globalThis.name = \"\";\n"
"globalThis.addEventListener = function (t, f) { document.addEventListener(t, f); };\n"
"globalThis.removeEventListener = function (t, f) { document.removeEventListener(t, f); };\n"
"globalThis.requestAnimationFrame = function (f) {\n"
"  return sf_setTimeout(function () { f(Date.now()); }, 16);\n"
"};\n"
"globalThis.cancelAnimationFrame = function (id) { sf_clearTimeout(id); };\n"
"globalThis.__k93_dcl = function () {\n"
"  var ev = { type: \"DOMContentLoaded\", target: document,\n"
"    currentTarget: document, defaultPrevented: false,\n"
"    preventDefault: function () { ev.defaultPrevented = true; },\n"
"    stopPropagation: function () {} };\n"
"  var ls = __doclsn.slice();\n"
"  for (var i = 0; i < ls.length; i++)\n"
"    if (ls[i].t === \"DOMContentLoaded\" || ls[i].t === \"load\") {\n"
"      try { ls[i].f.call(document, ev); } catch (e) { console.log(\"dcl: \" + e); }\n"
"    }\n"
"};\n";

/* ------------------------------------------------------------ lifecycle */

static void node_finalizer(JSRuntime *r, JSValue v)
{
    (void)r; (void)v;
}

static void js_error_log(const char *what)
{
    JSValue e = JS_GetException(ctx);
    const char *m = JS_ToCString(ctx, e);
    k93_log("safari-js", "%s: %s", what, m ? m : "?");
    JS_FreeCString(ctx, m);
    JS_FreeValue(ctx, e);
}

static void bind_global(JSContext *c, const char *name,
                        JSCFunction *fn, int argc)
{
    JSValue g = JS_GetGlobalObject(c);
    JS_SetPropertyStr(c, g, name, JS_NewCFunction(c, fn, name, argc));
    JS_FreeValue(c, g);
}

int js_open(sf_page *p, k93node *doc, const char *base)
{
    (void)base;
    PG = p;
    DOC = doc;
    dirty = 0;
    scripts_done = 0;
    navurl[0] = 0;
    cookiejar[0] = 0;
    memset(T, 0, sizeof T);
    nwrap = 0;
    rt = JS_NewRuntime();
    if (!rt) return -1;
    ctx = JS_NewContext(rt);
    if (!ctx) { JS_FreeRuntime(rt); rt = NULL; return -1; }

    JS_NewClassID(&node_class);
    static JSClassDef def = { .class_name = "Node", .finalizer = node_finalizer };
    JS_NewClass(rt, node_class, &def);
    JSValue proto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, proto, node_proto_list,
                               (int)(sizeof node_proto_list /
                                     sizeof node_proto_list[0]));
    JS_SetClassProto(ctx, node_class, proto);
    JSValue g = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, g, "__sf_proto", JS_DupValue(ctx, proto));

    bind_global(ctx, "sf_fetch", c_fetch, 1);
    bind_global(ctx, "sf_xhr", c_fetch2, 1);
    bind_global(ctx, "sf_log", c_log, 1);
    bind_global(ctx, "sf_base", c_base, 0);
    bind_global(ctx, "sf_nav", c_nav, 1);
    bind_global(ctx, "sf_eval", c_eval, 1);
    bind_global(ctx, "sf_alert", c_alert, 1);
    bind_global(ctx, "sf_getElementById", c_getElementById, 1);
    bind_global(ctx, "sf_query", c_query1, 1);
    bind_global(ctx, "sf_queryAll", c_queryAll, 1);
    bind_global(ctx, "sf_createElement", c_createElement, 1);
    bind_global(ctx, "sf_createTextNode", c_createTextNode, 1);
    bind_global(ctx, "sf_write", c_write, 1);
    bind_global(ctx, "sf_getTitle", c_getTitle, 0);
    bind_global(ctx, "sf_setTitle", c_setTitle, 1);
    bind_global(ctx, "sf_getCookie", c_cookie_get, 0);
    bind_global(ctx, "sf_setCookie", c_cookie_set, 1);
    bind_global(ctx, "sf_setTimeout", c_setTimeout, 2);
    bind_global(ctx, "sf_setInterval", c_setInterval, 2);
    bind_global(ctx, "sf_clearTimeout", c_clearTimer, 1);
    bind_global(ctx, "sf_readyState", c_readyState, 0);
    JS_FreeValue(ctx, g);

    JSValue r = JS_Eval(ctx, bootstrap_js, sizeof bootstrap_js - 1,
                        "k93-boot.js", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(r)) {
        js_error_log("bootstrap");
        JS_FreeValue(ctx, r);
        return -1;
    }
    JS_FreeValue(ctx, r);
    return 0;
}

void js_close(void)
{
    if (!rt) return;
    for (int i = 0; i < MAXTIMERS; i++)
        if (T[i].active) timer_clear(ctx, i + 1);
    for (int i = 0; i < nwrap; i++) JS_FreeValue(ctx, wrap[i].v);
    nwrap = 0;
    JS_FreeContext(ctx);
    ctx = NULL;
    JS_FreeRuntime(rt);
    rt = NULL;
    PG = NULL;
    DOC = NULL;
}

void js_run_scripts(k93node *doc)
{
    if (!ctx) return;
    k93node *out[128];
    int n = k93html_query_all(doc, "script", out, 128);
    int idx = 0;
    for (int i = 0; i < n; i++) {
        k93node *s = out[i];
        const char *type = k93html_attr(s, "type");
        if (type && *type && strcasestr(type, "javascript") == NULL &&
            strcasestr(type, "ecmascript") == NULL &&
            strcasecmp(type, "module"))
            continue;                     /* e.g. application/ld+json */
        char name[32];
        snprintf(name, sizeof name, "inline#%d", idx++);
        const char *src = k93html_attr(s, "src");
        JSValue r;
        if (src && *src) {
            char abs[1280];
            char err[64];
            int status = 0;
            url_absolutize(PG->base, src, abs, sizeof abs);
            char *code = sf_fetch(abs, &status, err, sizeof err);
            if (!code) {
                k93_log("safari-js", "cannot fetch %s", abs);
                continue;
            }
            r = JS_Eval(ctx, code, sf_fetch_len(), abs, JS_EVAL_TYPE_GLOBAL);
            free(code);
        } else {
            const char *code = s->text ? s->text : "";
            r = JS_Eval(ctx, code, strlen(code), name, JS_EVAL_TYPE_GLOBAL);
        }
        if (JS_IsException(r)) js_error_log(name);
        JS_FreeValue(ctx, r);
        drain_jobs();
    }
    scripts_done = 1;
    static const char dcl_call[] =
        "try{if(typeof __k93_dcl===\"function\")__k93_dcl();}"
        "catch(e){sf_log(\"dcl: \"+e);}";
    JSValue d = JS_Eval(ctx, dcl_call, sizeof dcl_call - 1,
                        "dcl", JS_EVAL_TYPE_GLOBAL);
    JS_FreeValue(ctx, d);
    drain_jobs();
}

int js_tick(void)
{
    if (!ctx) return 0;
    fire_timers();
    drain_jobs();
    if (dirty) { dirty = 0; return 1; }
    return 0;
}

int js_click(sf_link *l)
{
    if (!ctx || !l || !l->node) return 0;
    return dispatch_click(l->node);
}

const char *js_nav_url(void) { return navurl[0] ? navurl : NULL; }
void js_nav_clear(void) { navurl[0] = 0; }
int js_active(void) { return ctx != NULL; }
