/* js.c - QuickJS engine for Safari. One JS "Node" wrapper per DOM node
 * (cached so script identity works), DOM methods bound as C functions on a
 * prototype, document/location/fetch/XHR/timers/console in a JS bootstrap.
 * Listeners live JS-side (Map on wrapper identity). */
#define _GNU_SOURCE
#include "safari.h"
#include "k93util.h"
#include "k93font.h"
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
    const char *method = argc > 0 ? JS_ToCString(ctx, argv[0]) : NULL;
    const char *u = argc > 1 ? JS_ToCString(ctx, argv[1]) : NULL;
    const char *reqbody = argc > 2 ? JS_ToCString(ctx, argv[2]) : NULL;
    const char *ctype = argc > 3 ? JS_ToCString(ctx, argv[3]) : NULL;
    int status = 0;
    char err[64];
    char *body = u ? k93net_request(method ? method : "GET", u, reqbody,
                                    reqbody ? strlen(reqbody) : 0, ctype,
                                    NULL, &status, err, sizeof err) : NULL;
    if (method) JS_FreeCString(ctx, method);
    if (u) JS_FreeCString(ctx, u);
    if (reqbody) JS_FreeCString(ctx, reqbody);
    if (ctype) JS_FreeCString(ctx, ctype);
    JS_SetPropertyUint32(ctx, arr, 0, JS_NewInt32(ctx, status));
    if (body) {
        JS_SetPropertyUint32(ctx, arr, 1,
                             js_str_sanitized(c, body, k93net_last_len()));
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

/* ------------------------------------------------- dialogs + storage --- */

static JSValue c_confirm(JSContext *c, JSValueConst thisv, int argc,
                         JSValueConst *argv)
{
    (void)thisv;
    const char *m = argc > 0 ? JS_ToCString(ctx, argv[0]) : "Are you sure?";
    int r = ui_confirm("Confirm", m && *m ? m : "Are you sure?");
    if (m) JS_FreeCString(ctx, m);
    dirty = 1;
    return JS_NewBool(c, r);
}

static JSValue c_prompt(JSContext *c, JSValueConst thisv, int argc,
                        JSValueConst *argv)
{
    (void)thisv;
    const char *msg = argc > 0 ? JS_ToCString(ctx, argv[0]) : "";
    const char *dv = argc > 1 ? JS_ToCString(ctx, argv[1]) : NULL;
    char buf[512];
    k93_strlcpy(buf, dv && dv[0] ? dv : "", sizeof buf);
    int r = ui_input(msg ? msg : "Enter value", buf, sizeof buf);
    JSValue out;
    if (r == 0) out = JS_NewString(c, buf);          /* 0 = accepted */
    else out = JS_NULL;
    if (msg) JS_FreeCString(ctx, msg);
    if (dv) JS_FreeCString(ctx, dv);
    dirty = 1;
    return out;
}

static JSValue c_selectmenu(JSContext *c, JSValueConst thisv, int argc,
                            JSValueConst *argv)
{
    (void)thisv;
    if (argc < 2) return JS_NewInt32(c, -1);
    const char *title = JS_ToCString(ctx, argv[0]);
    char items[64][64];
    const char *ptrs[64];
    int n = 0;
    if (JS_IsArray(ctx, argv[1])) {
        JSValue lv = JS_GetPropertyStr(ctx, argv[1], "length");
        int32_t ln = 0;
        JS_ToInt32(ctx, &ln, lv);
        JS_FreeValue(ctx, lv);
        if (ln > 64) ln = 64;
        for (int32_t i = 0; i < ln; i++) {
            JSValue ev = JS_GetPropertyUint32(ctx, argv[1], (uint32_t)i);
            const char *s = JS_ToCString(ctx, ev);
            k93_strlcpy(items[n], s ? s : "", sizeof items[0]);
            if (s) JS_FreeCString(ctx, s);
            JS_FreeValue(ctx, ev);
            ptrs[n] = items[n];
            n++;
        }
    }
    int sel = 0;
    if (argc > 2) JS_ToInt32(ctx, &sel, argv[2]);
    if (sel < 0) sel = 0;
    if (n > 0 && sel >= n) sel = n - 1;
    int r = n > 0 ? ui_menu(title && *title ? title : "Select", ptrs, n, sel)
                  : -1;
    if (title) JS_FreeCString(ctx, title);
    dirty = 1;
    return JS_NewInt32(c, r);
}

/* web storage: keys prefixed with the origin by the JS side. localStorage is
 * persisted to /var/mobile/Library/Safari/localstorage.txt across navigations
 * and app restarts; sessionStorage shares the same store (ceiling). */
#define MAXSTORE 64
#define SF_STORE_FILE "/var/mobile/Library/Safari/localstorage.txt"
static struct { char k[160]; char v[512]; } STORE[MAXSTORE];
static int nstore, srot;

static void store_load(void)
{
    size_t len = 0;
    char *t = k93_read_file(SF_STORE_FILE, &len);
    if (!t) return;
    nstore = 0;
    char *p = t, *end = t + len;
    while (p < end && nstore < MAXSTORE) {
        char *nl = memchr(p, '\n', (size_t)(end - p));
        char *line = nl ? nl : end;
        char *tab = memchr(p, '\t', (size_t)(line - p));
        size_t kl = tab ? (size_t)(tab - p) : (size_t)(line - p);
        size_t vl = tab ? (size_t)(line - tab - 1) : 0;
        if (kl < sizeof STORE[0].k && vl < sizeof STORE[0].v) {
            memcpy(STORE[nstore].k, p, kl); STORE[nstore].k[kl] = 0;
            memcpy(STORE[nstore].v, tab ? tab + 1 : line, vl);
            STORE[nstore].v[vl] = 0;
            nstore++;
        }
        if (!nl) break;
        p = nl + 1;
    }
    free(t);
}

static void store_save(void)
{
    k93_mkdir_p("/var/mobile/Library/Safari", 0777);
    size_t cap = 1;
    for (int i = 0; i < nstore; i++)
        cap += strlen(STORE[i].k) + strlen(STORE[i].v) + 3;
    char *buf = k93_xmalloc(cap);
    if (!buf) return;
    size_t o = 0;
    for (int i = 0; i < nstore; i++) {
        if (!STORE[i].k[0]) continue;
        o += (size_t)snprintf(buf + o, cap - o, "%s\t%s\n",
                              STORE[i].k, STORE[i].v);
    }
    k93_write_file(SF_STORE_FILE, buf, o, 0);
    free(buf);
}

static JSValue c_store_get(JSContext *c, JSValueConst thisv, int argc,
                           JSValueConst *argv)
{
    (void)thisv;
    if (argc < 1) return JS_NULL;
    const char *k = JS_ToCString(ctx, argv[0]);
    if (!k) return JS_NULL;
    JSValue out = JS_NULL;
    for (int i = 0; i < nstore; i++)
        if (!strcmp(STORE[i].k, k)) { out = JS_NewString(c, STORE[i].v); break; }
    JS_FreeCString(ctx, k);
    return out;
}

static JSValue c_store_set(JSContext *c, JSValueConst thisv, int argc,
                           JSValueConst *argv)
{
    (void)thisv;
    if (argc < 1) return JS_UNDEFINED;
    const char *k = JS_ToCString(ctx, argv[0]);
    if (!k) return JS_UNDEFINED;
    int slot = -1;
    for (int i = 0; i < nstore; i++)
        if (!strcmp(STORE[i].k, k)) { slot = i; break; }
    if (argc < 2 || JS_IsNull(argv[1]) || JS_IsUndefined(argv[1])) {
        if (slot >= 0) STORE[slot].k[0] = 0;        /* delete */
    } else {
        const char *v = JS_ToCString(ctx, argv[1]);
        if (slot < 0) {
            if (nstore < MAXSTORE) slot = nstore++;
            else { slot = srot % MAXSTORE; srot++; }
        }
        k93_strlcpy(STORE[slot].k, k, sizeof STORE[0].k);
        k93_strlcpy(STORE[slot].v, v ? v : "", sizeof STORE[0].v);
        if (v) JS_FreeCString(ctx, v);
    }
    JS_FreeCString(ctx, k);
    return JS_UNDEFINED;
}

static JSValue c_pxw(JSContext *c, JSValueConst thisv, int argc,
                     JSValueConst *argv)
{
    (void)thisv; (void)argc; (void)argv;
    return JS_NewInt32(c, ui_px_width());
}

static JSValue c_pxh(JSContext *c, JSValueConst thisv, int argc,
                     JSValueConst *argv)
{
    (void)thisv; (void)argc; (void)argv;
    return JS_NewInt32(c, ui_px_height());
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
        if (!strcmp(n->tag, "#text")) return JS_NewInt32(c, 3);
        if (!strncmp(n->tag, "#comment", 8)) return JS_NewInt32(c, 8);
        return JS_NewInt32(c, 1);
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
    if (!strcmp(ch->tag, "#fragment")) {   /* move fragment children in order */
        k93node *f = ch->first;
        while (f) {
            k93node *nx = f->next;
            k93html_append(n, f);
            f = nx;
        }
        dirty = 1;
        return JS_DupValue(c, argv[0]);
    }
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

static JSValue n_matches(JSContext *c, JSValueConst thisv, int argc,
                         JSValueConst *argv)
{
    k93node *n = node_of(c, thisv);
    if (!n || argc < 1) return JS_FALSE;
    const char *sel = JS_ToCString(ctx, argv[0]);
    int m = sel ? k93html_matches(n, sel) : 0;
    if (sel) JS_FreeCString(ctx, sel);
    return JS_NewBool(c, m);
}

static JSValue n_closest(JSContext *c, JSValueConst thisv, int argc,
                         JSValueConst *argv)
{
    k93node *n = node_of(c, thisv);
    if (!n || argc < 1) return JS_NULL;
    const char *sel = JS_ToCString(ctx, argv[0]);
    k93node *hit = NULL;
    if (sel) {
        for (k93node *k = n; k && !hit; k = k->parent)
            if (k93html_matches(k, sel)) hit = k;
        JS_FreeCString(ctx, sel);
    }
    return node_wrap(c, hit);
}

/* insertAdjacentHTML: beforebegin/afterbegin/beforeend/afterend */
static JSValue n_insertAdjacentHTML(JSContext *c, JSValueConst thisv, int argc,
                                    JSValueConst *argv)
{
    k93node *n = node_of(c, thisv);
    if (!n || argc < 2) return JS_UNDEFINED;
    const char *pos = JS_ToCString(ctx, argv[0]);
    const char *html = JS_ToCString(ctx, argv[1]);
    if (!pos || !html) {
        if (pos) JS_FreeCString(ctx, pos);
        if (html) JS_FreeCString(ctx, html);
        return JS_UNDEFINED;
    }
    k93node *frag = k93html_parse(html);
    JS_FreeCString(ctx, pos);
    JS_FreeCString(ctx, html);
    if (!frag) return JS_UNDEFINED;
    if (!strcmp(pos, "beforeend")) {
        while (frag->first) k93html_append(n, frag->first);
    } else if (!strcmp(pos, "afterbegin")) {
        k93node *anchor = n->first;
        while (frag->first) {
            if (anchor) k93html_insert_before(anchor, frag->first);
            else k93html_append(n, frag->first);
        }
    } else if (!strcmp(pos, "beforebegin") && n->parent) {
        while (frag->first) k93html_insert_before(n, frag->first);
    } else if (!strcmp(pos, "afterend") && n->parent) {
        k93node *anchor = n->next;
        while (frag->first) {
            if (anchor) k93html_insert_before(anchor, frag->first);
            else k93html_append(n->parent, frag->first);
        }
    }
    frag->first = frag->last = NULL;
    free(frag->tag);
    free(frag);
    dirty = 1;
    return JS_UNDEFINED;
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

/* ---------------------------------------------------- canvas 2D (ctx) */
/* getContext("2d") surfaces live in PG->cv[] (page-owned pixels, opaque
 * white background). Real drawing: fillRect/strokeRect/clearRect,
 * fillText (TTF glyph coverage blended), moveTo/lineTo/stroke polyline,
 * rect/fill. Ceiling: fill() handles rects only, no transforms, no
 * curves, no canvas images; other contexts return null (no WebGL). */

static JSClassID ctx2d_class;

static void cv_dot(sf_cv *e, int x, int y, uint32_t col)
{
    if (!e->px || x < 0 || y < 0 || x >= e->w || y >= e->h) return;
    e->px[(size_t)y * e->w + x] = 0xFF000000u | (col & 0xFFFFFF);
}

static void cv_fillbox(sf_cv *e, int x, int y, int w, int h, uint32_t col)
{
    for (int yy = 0; yy < h; yy++)
        for (int xx = 0; xx < w; xx++)
            cv_dot(e, x + xx, y + yy, col);
}

static void cv_strokebox(sf_cv *e, int x, int y, int w, int h, uint32_t col)
{
    for (int xx = 0; xx < w; xx++) {
        cv_dot(e, x + xx, y, col);
        cv_dot(e, x + xx, y + h - 1, col);
    }
    for (int yy = 0; yy < h; yy++) {
        cv_dot(e, x, y + yy, col);
        cv_dot(e, x + w - 1, y + yy, col);
    }
}

static void cv_line(sf_cv *e, int x0, int y0, int x1, int y1, uint32_t col)
{
    int dx = x1 > x0 ? x1 - x0 : x0 - x1;
    int dy = y1 > y0 ? y1 - y0 : y0 - y1;
    int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
    int err = dx - dy;
    for (;;) {
        cv_dot(e, x0, y0, col);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 < dx) { err += dx; y0 += sy; }
    }
}

static uint32_t cv_style(JSContext *c, JSValueConst obj, const char *prop)
{
    uint32_t col = 0;
    JSValue v = JS_GetPropertyStr(c, obj, prop);
    if (!JS_IsException(v)) {
        const char *s = JS_ToCString(c, v);
        if (s) { k93css_color(s, &col); JS_FreeCString(c, s); }
        JS_FreeValue(c, v);
    }
    return col;
}

static sf_cv *cv_of(JSContext *c, JSValueConst thisv)
{
    return JS_GetOpaque2(c, thisv, ctx2d_class);
}

/* argv[0..3] = x, y, w, h; negative w/h normalized like browsers */
static void cv_rect_args(JSContext *c, JSValueConst *argv, int argc,
                         int *x, int *y, int *w, int *h)
{
    int64_t v;
    *x = *y = *w = *h = 0;
    if (argc > 0 && JS_ToInt64(c, &v, argv[0]) == 0) *x = (int)v;
    if (argc > 1 && JS_ToInt64(c, &v, argv[1]) == 0) *y = (int)v;
    if (argc > 2 && JS_ToInt64(c, &v, argv[2]) == 0) *w = (int)v;
    if (argc > 3 && JS_ToInt64(c, &v, argv[3]) == 0) *h = (int)v;
    if (*w < 0) { *x += *w; *w = -*w; }
    if (*h < 0) { *y += *h; *h = -*h; }
}

static JSValue c2_fillRect(JSContext *c, JSValueConst thisv, int argc,
                           JSValueConst *argv)
{
    sf_cv *e = cv_of(c, thisv);
    if (!e) return JS_UNDEFINED;
    int x, y, w, h;
    cv_rect_args(c, argv, argc, &x, &y, &w, &h);
    cv_fillbox(e, x, y, w, h, cv_style(c, thisv, "fillStyle"));
    dirty = 1;
    return JS_UNDEFINED;
}

static JSValue c2_clearRect(JSContext *c, JSValueConst thisv, int argc,
                            JSValueConst *argv)
{
    /* no alpha channel in the surface: clear to white (page background) */
    sf_cv *e = cv_of(c, thisv);
    if (!e) return JS_UNDEFINED;
    int x, y, w, h;
    cv_rect_args(c, argv, argc, &x, &y, &w, &h);
    cv_fillbox(e, x, y, w, h, 0xFFFFFF);
    dirty = 1;
    return JS_UNDEFINED;
}

static JSValue c2_strokeRect(JSContext *c, JSValueConst thisv, int argc,
                             JSValueConst *argv)
{
    sf_cv *e = cv_of(c, thisv);
    if (!e) return JS_UNDEFINED;
    int x, y, w, h;
    cv_rect_args(c, argv, argc, &x, &y, &w, &h);
    if (w > 0 && h > 0)
        cv_strokebox(e, x, y, w, h, cv_style(c, thisv, "strokeStyle"));
    dirty = 1;
    return JS_UNDEFINED;
}

static JSValue c2_fillText(JSContext *c, JSValueConst thisv, int argc,
                           JSValueConst *argv)
{
    sf_cv *e = cv_of(c, thisv);
    if (!e || argc < 1) return JS_UNDEFINED;
    const char *s = JS_ToCString(c, argv[0]);
    if (!s) return JS_UNDEFINED;
    int x = 0, y = 0;
    int64_t v;
    if (argc > 1 && JS_ToInt64(c, &v, argv[1]) == 0) x = (int)v;
    if (argc > 2 && JS_ToInt64(c, &v, argv[2]) == 0) y = (int)v;
    uint32_t col = cv_style(c, thisv, "fillStyle");
    k93font_select(k93font_default_id());
    int base = k93font_baseline(1);
    int pen = x;
    uint32_t prev = 0;
    for (int i = 0; s[i]; ) {
        uint32_t cp = k93_utf8_next(s, &i);
        int gw = 0, gh = 0, xo = 0, yo = 0;
        float adv = 0;
        uint8_t *bm = k93font_glyph(prev, cp, 1, 0, &gw, &gh, &xo, &yo, &adv);
        if (bm) {
            for (int gy = 0; gy < gh; gy++)
                for (int gx = 0; gx < gw; gx++) {
                    int cov = bm[(size_t)gy * gw + gx];
                    if (!cov) continue;
                    int dx = pen + xo + gx, dy = y - base + yo + gy;
                    if (dx < 0 || dy < 0 || dx >= e->w || dy >= e->h) continue;
                    uint32_t *d = &e->px[(size_t)dy * e->w + dx];
                    uint32_t dr = (*d >> 16) & 255, dg = (*d >> 8) & 255,
                             db = *d & 255;
                    uint32_t r = (((col >> 16) & 255) * cov + dr * (255 - cov)) / 255;
                    uint32_t g = (((col >> 8) & 255) * cov + dg * (255 - cov)) / 255;
                    uint32_t b = ((col & 255) * cov + db * (255 - cov)) / 255;
                    *d = 0xFF000000u | (r << 16) | (g << 8) | b;
                }
            k93font_glyph_free(bm);
        }
        pen += (int)adv;
        prev = cp;
    }
    JS_FreeCString(c, s);
    dirty = 1;
    return JS_UNDEFINED;
}

static JSValue c2_beginPath(JSContext *c, JSValueConst thisv, int argc,
                            JSValueConst *argv)
{
    (void)argc; (void)argv;
    sf_cv *e = cv_of(c, thisv);
    if (e) { e->np = 0; e->has_rect = 0; }
    return JS_UNDEFINED;
}

static void cv_pt(sf_cv *e, int x, int y)
{
    if (e->np < 64) { e->ptx[e->np] = x; e->pty[e->np] = y; e->np++; }
}

static int cv_xy(JSContext *c, JSValueConst *argv, int argc, int *x, int *y)
{
    int64_t v;
    *x = *y = 0;
    int ok = 0;
    if (argc > 0 && JS_ToInt64(c, &v, argv[0]) == 0) { *x = (int)v; ok++; }
    if (argc > 1 && JS_ToInt64(c, &v, argv[1]) == 0) { *y = (int)v; ok++; }
    return ok == 2;
}

static JSValue c2_moveTo(JSContext *c, JSValueConst thisv, int argc,
                         JSValueConst *argv)
{
    sf_cv *e = cv_of(c, thisv);
    int x, y;
    if (e && cv_xy(c, argv, argc, &x, &y)) { e->np = 0; cv_pt(e, x, y); }
    return JS_UNDEFINED;
}

static JSValue c2_lineTo(JSContext *c, JSValueConst thisv, int argc,
                         JSValueConst *argv)
{
    sf_cv *e = cv_of(c, thisv);
    int x, y;
    if (e && cv_xy(c, argv, argc, &x, &y)) cv_pt(e, x, y);
    return JS_UNDEFINED;
}

static JSValue c2_closePath(JSContext *c, JSValueConst thisv, int argc,
                            JSValueConst *argv)
{
    (void)argc; (void)argv;
    sf_cv *e = cv_of(c, thisv);
    if (e && e->np > 1) cv_pt(e, e->ptx[0], e->pty[0]);
    return JS_UNDEFINED;
}

static JSValue c2_stroke(JSContext *c, JSValueConst thisv, int argc,
                         JSValueConst *argv)
{
    (void)argc; (void)argv;
    sf_cv *e = cv_of(c, thisv);
    if (e && e->np > 1) {
        uint32_t col = cv_style(c, thisv, "strokeStyle");
        for (int i = 1; i < e->np; i++)
            cv_line(e, e->ptx[i - 1], e->pty[i - 1], e->ptx[i], e->pty[i], col);
        dirty = 1;
    }
    return JS_UNDEFINED;
}

static JSValue c2_rect(JSContext *c, JSValueConst thisv, int argc,
                       JSValueConst *argv)
{
    sf_cv *e = cv_of(c, thisv);
    if (!e) return JS_UNDEFINED;
    int x, y, w, h;
    cv_rect_args(c, argv, argc, &x, &y, &w, &h);
    if (w <= 0 || h <= 0) return JS_UNDEFINED;
    e->has_rect = 1;
    e->rx = x; e->ry = y; e->rw = w; e->rh = h;
    cv_pt(e, x, y);
    cv_pt(e, x + w, y);
    cv_pt(e, x + w, y + h);
    cv_pt(e, x, y + h);
    cv_pt(e, x, y);
    return JS_UNDEFINED;
}

static JSValue c2_fill(JSContext *c, JSValueConst thisv, int argc,
                       JSValueConst *argv)
{
    (void)argc; (void)argv;
    sf_cv *e = cv_of(c, thisv);
    if (e && e->has_rect) {            /* polygon fill: rects only (ceiling) */
        cv_fillbox(e, e->rx, e->ry, e->rw, e->rh,
                   cv_style(c, thisv, "fillStyle"));
        dirty = 1;
    }
    return JS_UNDEFINED;
}

static const JSCFunctionListEntry ctx2d_proto_list[] = {
    JS_CFUNC_DEF("fillRect", 4, c2_fillRect),
    JS_CFUNC_DEF("clearRect", 4, c2_clearRect),
    JS_CFUNC_DEF("strokeRect", 4, c2_strokeRect),
    JS_CFUNC_DEF("fillText", 3, c2_fillText),
    JS_CFUNC_DEF("beginPath", 0, c2_beginPath),
    JS_CFUNC_DEF("moveTo", 2, c2_moveTo),
    JS_CFUNC_DEF("lineTo", 2, c2_lineTo),
    JS_CFUNC_DEF("closePath", 0, c2_closePath),
    JS_CFUNC_DEF("stroke", 0, c2_stroke),
    JS_CFUNC_DEF("rect", 4, c2_rect),
    JS_CFUNC_DEF("fill", 0, c2_fill),
};

static JSValue n_getContext(JSContext *c, JSValueConst thisv, int argc,
                            JSValueConst *argv)
{
    k93node *n = node_of(c, thisv);
    if (!n || strcmp(n->tag, "canvas") || !PG) return JS_NULL;
    const char *type = argc > 0 ? JS_ToCString(c, argv[0]) : NULL;
    int is2d = type && !strcmp(type, "2d");
    if (type) JS_FreeCString(c, type);
    if (!is2d) return JS_NULL;                 /* no WebGL/other contexts */
    int idx = -1;
    for (int i = 0; i < PG->ncv; i++)
        if (PG->cv[i].node == n) { idx = i; break; }
    if (idx < 0) {
        if (PG->ncv >= SF_MAXCV) return JS_NULL;
        int w = 300, h = 150;                  /* spec defaults */
        const char *a = k93html_attr(n, "width");
        if (a && *a) w = atoi(a);
        a = k93html_attr(n, "height");
        if (a && *a) h = atoi(a);
        if (w < 1) w = 1;
        if (w > PG->width_px - 24) w = PG->width_px - 24;
        if (h < 1) h = 1;
        if (h > 400) h = 400;
        idx = PG->ncv++;
        sf_cv *e = &PG->cv[idx];
        memset(e, 0, sizeof *e);
        e->node = n;
        e->px = malloc((size_t)w * (size_t)h * 4);
        if (!e->px) { PG->ncv--; return JS_NULL; }
        for (size_t i = 0; i < (size_t)w * (size_t)h; i++)
            e->px[i] = 0xFFFFFFFFu;
        e->w = w;
        e->h = h;
    }
    JSValue obj = JS_NewObjectClass(c, ctx2d_class);
    if (JS_IsException(obj)) return JS_NULL;
    JS_SetOpaque(obj, &PG->cv[idx]);
    JS_SetPropertyStr(c, obj, "fillStyle", JS_NewString(c, "#000000"));
    JS_SetPropertyStr(c, obj, "strokeStyle", JS_NewString(c, "#000000"));
    return obj;
}

static const JSCFunctionListEntry node_proto_list[] = {
    JS_CFUNC_DEF("getContext", 1, n_getContext),
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
    JS_CFUNC_DEF("matches", 1, n_matches),
    JS_CFUNC_DEF("closest", 1, n_closest),
    JS_CFUNC_DEF("insertAdjacentHTML", 2, n_insertAdjacentHTML),
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
"var __ct = {}, __cc = {};\n"
"globalThis.console = { log: function () { sf_log([].slice.call(arguments).join(\" \")); },\n"
"  warn: function () { sf_log([].slice.call(arguments).join(\" \")); },\n"
"  error: function () { sf_log([].slice.call(arguments).join(\" \")); },\n"
"  info: function () { sf_log([].slice.call(arguments).join(\" \")); },\n"
"  debug: function () { sf_log([].slice.call(arguments).join(\" \")); },\n"
"  trace: function () { var s = (new Error()).stack || \"\"; sf_log(\"trace: \" + String(s).slice(0, 300)); },\n"
"  dir: function (o) { try { sf_log(\"dir: \" + JSON.stringify(o)); }\n"
"    catch (e) { sf_log(\"dir: \" + o); } },\n"
"  table: function (o) { if (Array.isArray(o)) {\n"
"      for (var i = 0; i < o.length; i++) { try { sf_log(\"table: \" + JSON.stringify(o[i])); }\n"
"        catch (e) { sf_log(\"table: \" + o[i]); } } }\n"
"    else sf_log(\"table: \" + o); },\n"
"  time: function (k) { __ct[k || \"default\"] = Date.now(); },\n"
"  timeEnd: function (k) { k = k || \"default\"; var t = __ct[k];\n"
"    sf_log(k + \": \" + (t ? (Date.now() - t) : 0) + \"ms\"); },\n"
"  count: function (k) { k = k || \"default\"; __cc[k] = (__cc[k] || 0) + 1;\n"
"    sf_log(k + \": \" + __cc[k]); },\n"
"  group: function () {}, groupCollapsed: function () {}, groupEnd: function () {},\n"
"  assert: function (c, m) { if (!c) sf_log(\"assert: \" + (m || \"failed\")); } };\n"
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
"function __k93_resolve(u) {\n"
"  u = String(u);\n"
"  if (/^[a-z][a-z0-9+.-]*:/i.test(u)) return u;\n"
"  var b = location.href.split(\"#\")[0];\n"
"  if (u.slice(0, 2) === \"//\") return b.split(\":\")[0] + \":\" + u;\n"
"  if (u.charAt(0) === \"#\") return b + u;\n"
"  if (u.charAt(0) === \"?\") return b.split(\"?\")[0] + u;\n"
"  if (u.charAt(0) === \"/\") { var m = b.match(/^([a-z]+:\\/\\/[^\\/?#]+)/i);\n"
"    return (m ? m[1] : \"\") + u; }\n"
"  var q = b.split(\"?\")[0];\n"
"  return q.slice(0, q.lastIndexOf(\"/\") + 1) + u;\n"
"}\n"
"var __b64 = \"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/\";\n"
"globalThis.btoa = function (s) {\n"
"  s = String(s);\n"
"  var out = \"\", i = 0;\n"
"  for (; i + 2 < s.length; i += 3) {\n"
"    var n = (s.charCodeAt(i) << 16) | (s.charCodeAt(i + 1) << 8) | s.charCodeAt(i + 2);\n"
"    out += __b64[(n >> 18) & 63] + __b64[(n >> 12) & 63] + __b64[(n >> 6) & 63] + __b64[n & 63];\n"
"  }\n"
"  if (i + 1 === s.length) { var a = s.charCodeAt(i);\n"
"    out += __b64[a >> 2] + __b64[(a << 4) & 63] + \"==\"; }\n"
"  else if (i + 2 === s.length) { var b = (s.charCodeAt(i) << 8) | s.charCodeAt(i + 1);\n"
"    out += __b64[b >> 10] + __b64[(b >> 4) & 63] + __b64[(b << 2) & 63] + \"=\"; }\n"
"  return out;\n"
"};\n"
"globalThis.atob = function (s) {\n"
"  s = String(s).replace(/[^A-Za-z0-9+/]/g, \"\");\n"
"  var out = \"\", i = 0;\n"
"  for (; i + 3 < s.length; i += 4) {\n"
"    var n = (__b64.indexOf(s[i]) << 18) | (__b64.indexOf(s[i + 1]) << 12) |\n"
"            (__b64.indexOf(s[i + 2]) << 6) | __b64.indexOf(s[i + 3]);\n"
"    out += String.fromCharCode((n >> 16) & 255, (n >> 8) & 255, n & 255);\n"
"  }\n"
"  var r = s.length - i;\n"
"  if (r === 2) { var m = (__b64.indexOf(s[i]) << 2) | (__b64.indexOf(s[i + 1]) >> 4);\n"
"    out += String.fromCharCode(m & 255); }\n"
"  else if (r === 3) { var p = (__b64.indexOf(s[i]) << 10) | (__b64.indexOf(s[i + 1]) << 4) |\n"
"    (__b64.indexOf(s[i + 2]) >> 2);\n"
"    out += String.fromCharCode((p >> 8) & 255, p & 255); }\n"
"  return out;\n"
"};\n"
"globalThis.queueMicrotask = function (f) { Promise.resolve().then(f); };\n"
"globalThis.confirm = function (m) { return !!sf_confirm(String(m == null ? \"\" : m)); };\n"
"globalThis.prompt = function (m, d) {\n"
"  return sf_prompt(String(m == null ? \"\" : m), String(d == null ? \"\" : d));\n"
"};\n"
"function __mkstore() {\n"
"  var pre = (function () { try { return location.origin; } catch (e) { return \"\"; } })();\n"
"  return {\n"
"    getItem: function (k) { return sf_store_get(pre + \"|\" + String(k)); },\n"
"    setItem: function (k, v) { sf_store_set(pre + \"|\" + String(k), String(v)); },\n"
"    removeItem: function (k) { sf_store_set(pre + \"|\" + String(k), null); },\n"
"    clear: function () {},\n"
"    key: function (i) { return null; },\n"
"    get length() { return 0; }\n"
"  };\n"
"}\n"
"/* ceiling: same RAM store for both; RAM only */\n"
"globalThis.localStorage = __mkstore();\n"
"globalThis.sessionStorage = globalThis.localStorage;\n"
"var __hist = [location.href], __hpos = 0;\n"
"globalThis.history = {\n"
"  get length() { return __hist.length; },\n"
"  get state() { return null; },\n"
"  pushState: function (s, t, u) { if (u != null) {\n"
"    __hist = __hist.slice(0, __hpos + 1); __hist.push(String(u));\n"
"    __hpos = __hist.length - 1; } },\n"
"  replaceState: function (s, t, u) { if (u != null) __hist[__hpos] = String(u); },\n"
"  back: function () { if (__hpos > 0) { __hpos--; sf_nav(__hist[__hpos]); } },\n"
"  forward: function () { if (__hpos + 1 < __hist.length) { __hpos++; sf_nav(__hist[__hpos]); } },\n"
"  go: function (d) { var p = __hpos + (d | 0);\n"
"    if (p >= 0 && p < __hist.length) { __hpos = p; sf_nav(__hist[p]); } }\n"
"};\n"
"Object.defineProperty(globalThis, \"innerWidth\",\n"
"  { get: function () { return sf_pxw(); }, configurable: true });\n"
"Object.defineProperty(globalThis, \"innerHeight\",\n"
"  { get: function () { return sf_pxh(); }, configurable: true });\n"
"Object.defineProperty(globalThis, \"outerWidth\",\n"
"  { get: function () { return sf_pxw(); }, configurable: true });\n"
"Object.defineProperty(globalThis, \"outerHeight\",\n"
"  { get: function () { return sf_pxh(); }, configurable: true });\n"
"Object.defineProperty(globalThis, \"devicePixelRatio\",\n"
"  { get: function () { return 1; }, configurable: true });\n"
"Object.defineProperty(globalThis, \"pageXOffset\",\n"
"  { get: function () { return 0; }, configurable: true });\n"
"Object.defineProperty(globalThis, \"pageYOffset\",\n"
"  { get: function () { return 0; }, configurable: true });\n"
"globalThis.scrollX = 0; globalThis.scrollY = 0;\n"
"globalThis.screen = { width: sf_pxw(), height: sf_pxh(),\n"
"  availWidth: sf_pxw(), availHeight: sf_pxh(), colorDepth: 24,\n"
"  pixelDepth: 24, orientation: { type: \"landscape-primary\" } };\n"
"globalThis.matchMedia = function (q) {\n"
"  return { matches: false, media: String(q), onchange: null,\n"
"    addListener: function () {}, removeListener: function () {},\n"
"    addEventListener: function () {}, removeEventListener: function () {},\n"
"    dispatchEvent: function () { return true; } };\n"
"};\n"
"var __cssd = { \"color\": \"rgb(0, 0, 0)\", \"background-color\": \"rgba(0, 0, 0, 0)\",\n"
"  \"display\": \"inline\", \"visibility\": \"visible\", \"font-size\": \"16px\",\n"
"  \"font-weight\": \"400\", \"font-family\": \"sans-serif\", \"opacity\": \"1\",\n"
"  \"position\": \"static\", \"overflow\": \"visible\" };\n"
"globalThis.getComputedStyle = function (el) {\n"
"  var inline = (el && el.getAttribute) ? (el.getAttribute(\"style\") || \"\") : \"\";\n"
"  var o = { getPropertyValue: function (k) {\n"
"    var m = inline.match(new RegExp(\"(?:^|;)\\\\s*\" + k.replace(/-/g, \"\\\\-\") +\n"
"      \"\\\\s*:\\\\s*([^;]*)\", \"i\"));\n"
"    if (m) return m[1].trim();\n"
"    return (k in __cssd) ? __cssd[k] : \"\";\n"
"  } };\n"
"  return new Proxy(o, { get: function (t, k) {\n"
"    if (typeof k !== \"string\" || k in t) return t[k];\n"
"    return t.getPropertyValue(k.replace(/[A-Z]/g,\n"
"      function (m) { return \"-\" + m.toLowerCase(); }));\n"
"  } });\n"
"};\n"
"globalThis.Image = function (w, h) {\n"
"  var i = document.createElement(\"img\");\n"
"  if (w != null) i.setAttribute(\"width\", String(w));\n"
"  if (h != null) i.setAttribute(\"height\", String(h));\n"
"  return i;\n"
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
"P.dispatchEvent = function (ev) {\n"
"  if (!ev || !ev.type) return true;\n"
"  __k93_emit(this, String(ev.type), ev);\n"
"  return !ev.defaultPrevented;\n"
"};\n"
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
"P.hasAttribute = function (k) { return this.getAttribute(k) != null; };\n"
"P.toggleAttribute = function (k, force) {\n"
"  var want = force === undefined ? !this.hasAttribute(k) : !!force;\n"
"  if (want) this.setAttribute(k, \"\"); else this.removeAttribute(k);\n"
"  return want;\n"
"};\n"
"Object.defineProperty(P, \"lastChild\", { get: function () {\n"
"  var c = this.firstChild, l = null; while (c) { l = c; c = c.nextSibling; }\n"
"  return l; } });\n"
"Object.defineProperty(P, \"firstElementChild\", { get: function () {\n"
"  var c = this.firstChild; while (c && c.nodeType === 3) c = c.nextSibling;\n"
"  return c; } });\n"
"Object.defineProperty(P, \"lastElementChild\", { get: function () {\n"
"  var e = this.children; return e && e.length ? e[e.length - 1] : null; } });\n"
"Object.defineProperty(P, \"nextElementSibling\", { get: function () {\n"
"  var c = this.nextSibling; while (c && c.nodeType === 3) c = c.nextSibling;\n"
"  return c; } });\n"
"Object.defineProperty(P, \"previousElementSibling\", { get: function () {\n"
"  var c = this.previousSibling; while (c && c.nodeType === 3) c = c.previousSibling;\n"
"  return c; } });\n"
"Object.defineProperty(P, \"childElementCount\", { get: function () {\n"
"  return this.children.length; } });\n"
"function __attrprop(name, dflt) {\n"
"  return { get: function () { var v = this.getAttribute(name);\n"
"      return v == null ? (dflt === undefined ? \"\" : dflt) : v; },\n"
"    set: function (v) { this.setAttribute(name, String(v)); } };\n"
"}\n"
"function __boolprop(name, radio) {\n"
"  return { get: function () { return this.hasAttribute(name); },\n"
"    set: function (v) {\n"
"      if (radio && v) {\n"
"        var nm = this.getAttribute(\"name\");\n"
"        if (nm) {\n"
"          var root = this.form || document;\n"
"          var all = root.querySelectorAll(\"input\");\n"
"          for (var i = 0; i < all.length; i++) {\n"
"            var a = all[i];\n"
"            if (a !== this && (a.getAttribute(\"type\") || \"\").toLowerCase() === \"radio\" &&\n"
"                a.getAttribute(\"name\") === nm) a.removeAttribute(\"checked\");\n"
"          }\n"
"        }\n"
"      }\n"
"      if (v) this.setAttribute(name, \"\"); else this.removeAttribute(name);\n"
"    } };\n"
"}\n"
"Object.defineProperty(P, \"disabled\", __boolprop(\"disabled\"));\n"
"Object.defineProperty(P, \"checked\", __boolprop(\"checked\", true));\n"
"Object.defineProperty(P, \"selected\", __boolprop(\"selected\"));\n"
"Object.defineProperty(P, \"name\", __attrprop(\"name\"));\n"
"Object.defineProperty(P, \"placeholder\", __attrprop(\"placeholder\"));\n"
"Object.defineProperty(P, \"src\", __attrprop(\"src\"));\n"
"Object.defineProperty(P, \"alt\", __attrprop(\"alt\"));\n"
"Object.defineProperty(P, \"title\", __attrprop(\"title\"));\n"
"Object.defineProperty(P, \"target\", __attrprop(\"target\"));\n"
"Object.defineProperty(P, \"method\", __attrprop(\"method\", \"get\"));\n"
"Object.defineProperty(P, \"type\", { get: function () {\n"
"  var t = this.getAttribute(\"type\"); if (t) return t;\n"
"  var tag = (this.tagName || \"\").toLowerCase();\n"
"  if (tag === \"input\") return \"text\";\n"
"  if (tag === \"button\") return \"submit\";\n"
"  if (tag === \"select\") return \"select-one\";\n"
"  if (tag === \"textarea\") return \"textarea\";\n"
"  return \"\"; },\n"
"  set: function (v) { this.setAttribute(\"type\", String(v)); } });\n"
"Object.defineProperty(P, \"href\", { get: function () {\n"
"  var h = this.getAttribute(\"href\"); return h == null ? \"\" : __k93_resolve(h); },\n"
"  set: function (v) { this.setAttribute(\"href\", String(v)); } });\n"
"Object.defineProperty(P, \"action\", { get: function () {\n"
"  var a = this.getAttribute(\"action\");\n"
"  return a == null ? location.href : __k93_resolve(a); },\n"
"  set: function (v) { this.setAttribute(\"action\", String(v)); } });\n"
"Object.defineProperty(P, \"value\", { get: function () {\n"
"  var tag = (this.tagName || \"\").toLowerCase();\n"
"  if (tag === \"select\") {\n"
"    var opts = this.querySelectorAll(\"option\");\n"
"    for (var i = 0; i < opts.length; i++)\n"
"      if (opts[i].hasAttribute(\"selected\")) return opts[i].value;\n"
"    return opts.length ? opts[0].value : \"\";\n"
"  }\n"
"  if (tag === \"option\" || tag === \"textarea\") {\n"
"    var tv = this.getAttribute(\"value\");\n"
"    if (tv != null) return tv;\n"
"    return (this.textContent || \"\").replace(/^\\s+|\\s+$/g, \"\");\n"
"  }\n"
"  var iv = this.getAttribute(\"value\");\n"
"  return iv == null ? \"\" : iv;\n"
"},\n"
"  set: function (v) {\n"
"    v = String(v);\n"
"    var tag = (this.tagName || \"\").toLowerCase();\n"
"    if (tag === \"select\") {\n"
"      var opts = this.querySelectorAll(\"option\");\n"
"      for (var i = 0; i < opts.length; i++) {\n"
"        if (opts[i].value === v) opts[i].setAttribute(\"selected\", \"\");\n"
"        else opts[i].removeAttribute(\"selected\");\n"
"      }\n"
"      return;\n"
"    }\n"
"    this.setAttribute(\"value\", v);\n"
"  } });\n"
"Object.defineProperty(P, \"defaultValue\", __attrprop(\"value\"));\n"
"Object.defineProperty(P, \"selectedIndex\", { get: function () {\n"
"  var opts = this.querySelectorAll(\"option\");\n"
"  for (var i = 0; i < opts.length; i++)\n"
"    if (opts[i].hasAttribute(\"selected\")) return i;\n"
"  return opts.length ? 0 : -1;\n"
"},\n"
"  set: function (i) {\n"
"    var opts = this.querySelectorAll(\"option\");\n"
"    i = i | 0;\n"
"    for (var j = 0; j < opts.length; j++) {\n"
"      if (j === i) opts[j].setAttribute(\"selected\", \"\");\n"
"      else opts[j].removeAttribute(\"selected\");\n"
"    }\n"
"  } });\n"
"Object.defineProperty(P, \"options\", { get: function () {\n"
"  return this.querySelectorAll(\"option\"); } });\n"
"Object.defineProperty(P, \"form\", { get: function () {\n"
"  var n = this;\n"
"  while (n) { if ((n.tagName || \"\").toLowerCase() === \"form\") return n;\n"
"    n = n.parentNode; }\n"
"  return null; } });\n"
"Object.defineProperty(P, \"elements\", { get: function () {\n"
"  return this.querySelectorAll(\"input, select, textarea, button\"); } });\n"
"P.submit = function () { __k93_submit(this); };\n"
"P.reset = function () { __k93_reset(this); };\n"
/* ---- event core: bubble node -> ancestors -> document, then default ---- */
"function __k93_emit(target, type, base) {\n"
"  var ev = base || {};\n"
"  ev.type = type;\n"
"  if (ev.target === undefined || ev.target === null) ev.target = target;\n"
"  ev.bubbles = ev.bubbles !== false;\n"
"  ev.cancelable = ev.cancelable !== false;\n"
"  ev.defaultPrevented = !!ev.defaultPrevented;\n"
"  ev._stop = false;\n"
"  ev._imm = false;\n"
"  ev.eventPhase = 2;\n"
"  if (!ev.preventDefault) ev.preventDefault = function () { ev.defaultPrevented = true; };\n"
"  if (!ev.stopPropagation) ev.stopPropagation = function () { ev._stop = true; };\n"
"  if (!ev.stopImmediatePropagation)\n"
"    ev.stopImmediatePropagation = function () { ev._stop = true; ev._imm = true; };\n"
"  var chain = [], n = target;\n"
"  while (n) { chain.push(n); n = n.parentNode; }\n"
"  if (chain[chain.length - 1] !== document) chain.push(document);\n"
"  for (var ci = 0; ci < chain.length && !ev._stop; ci++) {\n"
"    var t = chain[ci];\n"
"    ev.currentTarget = t;\n"
"    var ls = __listeners.get(t);\n"
"    if (ls) {\n"
"      ls = ls.slice();\n"
"      for (var i = 0; i < ls.length && !ev._stop; i++) {\n"
"        if (ls[i].t !== type) continue;\n"
"        try { ls[i].f.call(t, ev); } catch (e) { console.log(type + \": \" + e); }\n"
"        if (ev._imm) break;\n"
"      }\n"
"    }\n"
"    if (ev._imm) break;\n"
"    if (t && t !== document && t.getAttribute) {\n"
"      var oc = t.getAttribute(\"on\" + type);\n"
"      if (oc) {\n"
"        try { (new Function(\"event\", oc)).call(t, ev); }\n"
"        catch (e) { console.log(\"on\" + type + \": \" + e); }\n"
"        if (ev._imm) break;\n"
"      }\n"
"    }\n"
"  }\n"
"  ev.currentTarget = null;\n"
"  ev.eventPhase = 0;\n"
"  return ev;\n"
"}\n"
/* ---- form submission: GET query built from named controls (POST ceiling) -- */
"function __k93_submit(form) {\n"
"  if (!form || !form.tagName || form.tagName.toLowerCase() !== \"form\") return;\n"
"  var ev = __k93_emit(form, \"submit\");\n"
"  if (ev.defaultPrevented) return;\n"
"  var method = (form.getAttribute(\"method\") || \"get\").toLowerCase();\n"
"  var action = form.getAttribute(\"action\") || location.href;\n"
"  var q = [];\n"
"  var cs = form.querySelectorAll(\"input, select, textarea\");\n"
"  for (var i = 0; i < cs.length; i++) {\n"
"    var c = cs[i];\n"
"    var t = c.type;\n"
"    if (t === \"submit\" || t === \"button\" || t === \"reset\" ||\n"
"        t === \"image\" || t === \"file\") continue;\n"
"    if (c.disabled) continue;\n"
"    var nm = c.getAttribute(\"name\");\n"
"    if (!nm) continue;\n"
"    q.push(encodeURIComponent(nm) + \"=\" + encodeURIComponent(c.value == null ? \"\" : c.value));\n"
"  }\n"
"  if (method !== \"get\") console.log(\"form POST: ceiling, sending as GET\");\n"
"  var url = __k93_resolve(action).split(\"#\")[0].split(\"?\")[0];\n"
"  var qs = q.join(\"&\");\n"
"  sf_nav(qs ? url + \"?\" + qs : url);\n"
"}\n"
"function __k93_reset(form) {\n"
"  if (!form || !form.elements) return;\n"
"  var cs = form.elements;\n"
"  for (var i = 0; i < cs.length; i++) {\n"
"    var c = cs[i], tag = (c.tagName || \"\").toLowerCase();\n"
"    if (c.__dc === undefined) c.__dc = c.hasAttribute(\"checked\");\n"
"    if (c.__dc) c.setAttribute(\"checked\", \"\"); else c.removeAttribute(\"checked\");\n"
"    if (c.__dv === undefined) c.__dv = c.getAttribute(\"value\");\n"
"    if (tag === \"textarea\") c.removeAttribute(\"value\");\n"
"    else if (c.__dv !== undefined) {\n"
"      if (c.__dv != null) c.setAttribute(\"value\", c.__dv);\n"
"      else c.removeAttribute(\"value\");\n"
"    }\n"
"    if (tag === \"select\") {\n"
"      if (c.__dsel === undefined) c.__dsel = c.selectedIndex;\n"
"      var opts = c.querySelectorAll(\"option\");\n"
"      for (var j = 0; j < opts.length; j++) {\n"
"        if (j === c.__dsel) opts[j].setAttribute(\"selected\", \"\");\n"
"        else opts[j].removeAttribute(\"selected\");\n"
"      }\n"
"    }\n"
"  }\n"
"  __k93_emit(form, \"reset\");\n"
"}\n"
/* ---- widget default actions (checkbox/radio/editor/menu/submit) ---------- */
"function __k93_default(node) {\n"
"  if (!node || !node.tagName || node.nodeType !== 1) return false;\n"
"  var tag = node.tagName.toLowerCase();\n"
"  var type = node.type;\n"
"  if (tag === \"input\") {\n"
"    if (type === \"checkbox\") {\n"
"      node.checked = !node.hasAttribute(\"checked\");\n"
"      __k93_emit(node, \"change\");\n"
"      return true;\n"
"    }\n"
"    if (type === \"radio\") { node.checked = true; __k93_emit(node, \"change\"); return true; }\n"
"    if (type === \"submit\" || type === \"image\") { __k93_submit(node.form); return true; }\n"
"    if (type === \"reset\") { __k93_reset(node.form); return true; }\n"
"    if (type === \"button\" || type === \"hidden\") return false;\n"
"    if (node.__dv === undefined) node.__dv = node.getAttribute(\"value\");\n"
"    var lab = node.getAttribute(\"placeholder\") || node.getAttribute(\"name\") || \"Text\";\n"
"    var nv = sf_prompt(lab, node.value == null ? \"\" : String(node.value));\n"
"    if (nv !== null && nv !== undefined) {\n"
"      node.value = nv;\n"
"      __k93_emit(node, \"input\");\n"
"      __k93_emit(node, \"change\");\n"
"    }\n"
"    return true;\n"
"  }\n"
"  if (tag === \"textarea\") {\n"
"    if (node.__dv === undefined) node.__dv = node.getAttribute(\"value\");\n"
"    var lab2 = node.getAttribute(\"placeholder\") || node.getAttribute(\"name\") || \"Text\";\n"
"    var nv2 = sf_prompt(lab2, String(node.value));\n"
"    if (nv2 !== null && nv2 !== undefined) {\n"
"      node.value = nv2;\n"
"      __k93_emit(node, \"input\");\n"
"      __k93_emit(node, \"change\");\n"
"    }\n"
"    return true;\n"
"  }\n"
"  if (tag === \"select\") {\n"
"    var opts = node.querySelectorAll(\"option\");\n"
"    if (!opts.length) return true;\n"
"    if (node.__dsel === undefined) node.__dsel = node.selectedIndex;\n"
"    var labels = [], cur = node.selectedIndex;\n"
"    for (var i = 0; i < opts.length; i++) {\n"
"      var ot = opts[i].textContent || \"\";\n"
"      labels.push(String(ot).replace(/^\\s+|\\s+$/g, \"\"));\n"
"    }\n"
"    var title = node.getAttribute(\"name\") || \"Select\";\n"
"    var pick = sf_selectmenu(title, labels, cur < 0 ? 0 : cur);\n"
"    if (pick >= 0 && pick < opts.length) {\n"
"      for (var j = 0; j < opts.length; j++) {\n"
"        if (j === pick) opts[j].setAttribute(\"selected\", \"\");\n"
"        else opts[j].removeAttribute(\"selected\");\n"
"      }\n"
"      __k93_emit(node, \"change\");\n"
"    }\n"
"    return true;\n"
"  }\n"
"  if (tag === \"button\") {\n"
"    if (type === \"reset\") { __k93_reset(node.form); return true; }\n"
"    if (type === \"submit\") { __k93_submit(node.form); return true; }\n"
"    return true;\n"
"  }\n"
"  return false;\n"
"}\n"
"globalThis.__k93_click = function (node) {\n"
"  var ev = __k93_emit(node, \"click\", { type: \"click\", target: node,\n"
"    bubbles: true, cancelable: true });\n"
"  if (!ev.defaultPrevented) __k93_default(node);\n"
"  return ev.defaultPrevented ? 2 : 1;\n"
"};\n"
"function XMLHttpRequest() { this.readyState = 0; this.status = 0;\n"
"  this.responseText = \"\"; this.onreadystatechange = null; this._m = \"GET\"; this._u = \"\"; }\n"
"XMLHttpRequest.prototype.open = function (m, u) {\n"
"  this._m = String(m || \"GET\"); this._u = String(u); this.readyState = 1; };\n"
"XMLHttpRequest.prototype.send = function (b) {\n"
"  var a = sf_xhr(this._m, this._u, b == null ? null : String(b));\n"
"  this.status = a[0]; this.responseText = a[1]; this.readyState = 4;\n"
"  if (this.onreadystatechange) this.onreadystatechange();\n"
"};\n"
"XMLHttpRequest.prototype.setRequestHeader = function () {};\n"
"XMLHttpRequest.prototype.getAllResponseHeaders = function () { return \"\"; };\n"
"globalThis.XMLHttpRequest = XMLHttpRequest;\n"
"function __resp(u, o) { o = o || {}; var a = sf_xhr(\n"
"  o.method || \"GET\", String(u), o.body == null ? null : String(o.body)); return {\n"
"  ok: a[0] >= 200 && a[0] < 300, status: a[0], url: u,\n"
"  text: function () { return Promise.resolve(a[1]); },\n"
"  json: function () { try { return Promise.resolve(JSON.parse(a[1])); }\n"
"    catch (e) { return Promise.reject(e); } },\n"
"  headers: { get: function () { return \"\"; } } }; }\n"
"globalThis.fetch = function (u, o) { return Promise.resolve(__resp(u, o)); };\n"
"globalThis.setTimeout = function (f, ms) { return sf_setTimeout(f, ms || 0); };\n"
"globalThis.clearTimeout = function (id) { sf_clearTimeout(id); };\n"
"globalThis.setInterval = function (f, ms) { return sf_setInterval(f, ms || 0); };\n"
"globalThis.clearInterval = function (id) { sf_clearTimeout(id); };\n"
"globalThis.alert = function (m) { sf_alert(String(m)); };\n"
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
"  createComment: function (t) {\n"
"    var c = document.createElement(\"#comment\"); c.textContent = String(t); return c; },\n"
"  createDocumentFragment: function () { return document.createElement(\"#fragment\"); },\n"
"  write: function (h) { sf_write(h); },\n"
"  writeln: function (h) { sf_write(h + \"\\n\"); },\n"
"  open: function () {}, close: function () {},\n"
"  get title() { return sf_getTitle(); },\n"
"  set title(v) { sf_setTitle(String(v)); },\n"
"  get cookie() { return sf_getCookie(); },\n"
"  set cookie(v) { sf_setCookie(String(v)); },\n"
"  addEventListener: function (t, f) {\n"
"    if (typeof f !== \"function\") return;\n"
"    var a = __listeners.get(document);\n"
"    if (!a) { a = []; __listeners.set(document, a); }\n"
"    for (var i = 0; i < a.length; i++) if (a[i].t === t && a[i].f === f) return;\n"
"    a.push({ t: t, f: f });\n"
"  },\n"
"  removeEventListener: function (t, f) {\n"
"    var a = __listeners.get(document); if (!a) return;\n"
"    for (var i = 0; i < a.length; i++)\n"
"      if (a[i].t === t && a[i].f === f) { a.splice(i, 1); return; }\n"
"  },\n"
"  get forms() { return this.querySelectorAll(\"form\"); },\n"
"  get defaultView() { return globalThis; },\n"
"  get activeElement() { return document.body; },\n"
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
"  var ls = __listeners.get(document);\n"
"  ls = ls ? ls.slice() : [];\n"
"  var ev = { type: \"DOMContentLoaded\", target: document,\n"
"    currentTarget: document, defaultPrevented: false,\n"
"    preventDefault: function () { ev.defaultPrevented = true; },\n"
"    stopPropagation: function () {} };\n"
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
    store_load();
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

    JS_NewClassID(&ctx2d_class);
    static JSClassDef c2def = { .class_name = "CanvasRenderingContext2D" };
    JS_NewClass(rt, ctx2d_class, &c2def);
    JSValue c2proto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, c2proto, ctx2d_proto_list,
                               (int)(sizeof ctx2d_proto_list /
                                     sizeof ctx2d_proto_list[0]));
    JS_SetClassProto(ctx, ctx2d_class, c2proto);

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
    bind_global(ctx, "sf_confirm", c_confirm, 1);
    bind_global(ctx, "sf_prompt", c_prompt, 2);
    bind_global(ctx, "sf_selectmenu", c_selectmenu, 3);
    bind_global(ctx, "sf_store_get", c_store_get, 1);
    bind_global(ctx, "sf_store_set", c_store_set, 2);
    bind_global(ctx, "sf_pxw", c_pxw, 0);
    bind_global(ctx, "sf_pxh", c_pxh, 0);
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
    store_save();
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

int js_run_url(const char *url)
{
    if (!ctx || !url || strncmp(url, "javascript:", 11)) return 0;
    const char *code = url + 11;
    JSValue r = JS_Eval(ctx, code, strlen(code), "js-url", JS_EVAL_TYPE_GLOBAL);
    int ok = !JS_IsException(r);
    if (!ok) {
        JSValue e = JS_GetException(ctx);
        const char *m = JS_ToCString(ctx, e);
        k93_log("safari-js", "javascript: URL error: %s", m ? m : "?");
        JS_FreeCString(ctx, m);
        JS_FreeValue(ctx, e);
    }
    JS_FreeValue(ctx, r);
    return ok;
}
