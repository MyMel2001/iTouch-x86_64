/* k93html.h - clean-room HTML tree parser + CSS subset (Safari engine).
 * Tag/attr names lowercase; text node tag = "#text"; comments dropped.
 * Entity decoding (named basics + numeric) happens at parse time.
 * CSS subset: tag / .class / #id / compound / descendant selectors;
 * properties: color, background-color, font-weight, font-size, text-align,
 * display:none, visibility:hidden, margin-left, padding-left. */
#ifndef K93HTML_H
#define K93HTML_H
#include <stddef.h>
#include <stdint.h>

typedef struct k93node k93node;
struct k93node {
    char *tag;                        /* lowercase, or "#text" */
    char *text;                       /* #text content (entities decoded) */
    int nattr;
    char **ak, **av;                  /* lowercase keys, decoded values */
    k93node *parent, *first, *last, *next, *prev;
};

k93node *k93html_parse(const char *html);
void     k93html_free(k93node *n);
k93node *k93html_elem(const char *tag);
k93node *k93html_textnode(const char *s);
void     k93html_append(k93node *p, k93node *c);
void     k93html_insert_before(k93node *ref, k93node *n);
void     k93html_unlink(k93node *n);
k93node *k93html_clone(const k93node *n);
const char *k93html_attr(const k93node *n, const char *k);
void     k93html_set_attr(k93node *n, const char *k, const char *v);
k93node *k93html_get_id(k93node *root, const char *id);
k93node *k93html_query(k93node *root, const char *sel);        /* first match */
int      k93html_query_all(k93node *root, const char *sel,
                           k93node **out, int max);
void     k93html_text_content(const k93node *n, char *buf, size_t cap);
char    *k93html_inner_html(const k93node *n);                 /* malloc'd */
void     k93html_set_inner_html(k93node *n, const char *html);
void     k93html_entity_decode(char *s);

/* CSS subset */
typedef struct k93css k93css;
k93css *k93css_new(void);
void    k93css_free(k93css *c);
void    k93css_add(k93css *c, const char *text);

enum {
    K93CSS_FG = 1, K93CSS_BG = 2, K93CSS_BOLD = 4, K93CSS_CENTER = 8,
    K93CSS_HIDDEN = 16, K93CSS_SCALE = 32, K93CSS_INDENT = 64,
    K93CSS_UNDERLINE = 128, K93CSS_FONT = 256
};
typedef struct {
    uint32_t fg, bg;
    int bold, center, hidden, scale, indent;
    int underline;                    /* text-decoration: underline */
    int font;                         /* k93font id from font-family, -1 = none */
    unsigned flags;                   /* K93CSS_* of set properties */
} k93cstyle;

int  k93css_color(const char *v, uint32_t *out);   /* #rgb/#rrggbb/names */
void k93css_apply(const k93css *c, const k93node *n, k93cstyle *st);

#endif
