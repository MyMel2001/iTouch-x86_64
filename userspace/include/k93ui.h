/* k93ui.h - tiny cross-backend UI toolkit.
 * Backends: Linux framebuffer (/dev/fb0, vesafb) with an embedded 8x16 font,
 * or plain ANSI text on a VT/serial console. Input: stdin/console tty +
 * /dev/input/event0 raw keys. Single foreground app at a time (like
 * SpringBoard's frontmost application). */
#ifndef K93UI_H
#define K93UI_H
#include <stddef.h>
#include <stdint.h>

int  ui_init(const char *appname);       /* picks backend; returns 0 */
void ui_shutdown(void);
int  ui_width(void);                     /* columns */
int  ui_height(void);                    /* rows */
int  ui_is_graphical(void);
int  ui_px_width(void);                  /* logical canvas, pixels */
int  ui_px_height(void);
int  ui_display_modes(char out[][32], int max);  /* connector modes "WxH" */
void ui_clear(void);
void ui_flush(void);
void ui_puts(int x, int y, const char *s);              /* white on default bg */
void ui_puts_inv(int x, int y, const char *s);          /* inverted (selection) */
void ui_printf(int x, int y, const char *fmt, ...);
void ui_hline(int y, int attr);
void ui_statusbar(const char *title);    /* draws top bar with clock/battery */
void ui_statusmsg(const char *msg);      /* bottom bar message */

/* --- pixel graphics: only real on the graphical (DRM) backend; the text
 * console no-ops these and reports cell-grid px sizes, so apps that check
 * ui_is_graphical() get pixels and the rest keep working. Colors are packed
 * 0x00RRGGBB (alpha handled by ui_blit for icon data). */
uint32_t ui_rgb(uint8_t r, uint8_t g, uint8_t b);
uint32_t ui_tint(uint32_t c, int num, int den);   /* c*num/den, clamped: one
                                                     base color -> variants */
void ui_fill(int x, int y, int w, int h, uint32_t rgb);
void ui_fill_grad(int x, int y, int w, int h, uint32_t top, uint32_t bot);
void ui_roundrect(int x, int y, int w, int h, int r, uint32_t top, uint32_t bot);
void ui_frame(int x, int y, int w, int h, int r, int thick, uint32_t rgb);
void ui_blit(int x, int y, int w, int h, const uint32_t *argb); /* straight alpha */
void ui_blit_scaled(int x, int y, int dw, int dh,
                    const uint32_t *argb, int sw, int sh); /* nearest-neighbour */

/* --- image files (stb_image backend): PNG/JPG/GIF/BMP/TGA ---
 * Pixels are opaque 0xFFRRGGBB (alpha composited over white), drawn with
 * ui_blit/ui_blit_scaled. */
typedef struct { uint32_t *px; int w, h; } ui_image;
int  ui_image_load(ui_image *im, const char *path);  /* 0 ok, -1 error */
void ui_image_free(ui_image *im);                    /* safe on zeroed */
void ui_text(int x, int y, int scale, const char *s, uint32_t fg);
void ui_text_sh(int x, int y, int scale, const char *s, uint32_t fg, uint32_t shadow);
void ui_text_bold(int x, int y, int scale, const char *s, uint32_t fg, uint32_t shadow);
int  ui_text_width(int scale, const char *s);
int  ui_set_font(const char *family); /* CSS alias or family name; NULL/""/
                                       * "system" = default system font;
                                       * unknown name: -1, family kept */

/* events */
enum { UI_KEY_NONE = 0, UI_KEY_UP, UI_KEY_DOWN, UI_KEY_LEFT, UI_KEY_RIGHT,
       UI_KEY_ENTER, UI_KEY_ESC, UI_KEY_BACKSPACE, UI_KEY_TAB, UI_KEY_PGUP,
       UI_KEY_PGDN, UI_KEY_HOME, UI_KEY_END, UI_KEY_DELETE, UI_KEY_CTRL_C,
       UI_KEY_F12, UI_KEY_SWITCHER,
       UI_KEY_CHAR_BASE = 256 };         /* + unicode codepoint for printable */
int  ui_getkey(int timeout_ms);          /* -1 on timeout */

/* mouse (graphical backend; inert on the text console). Buttons:
 * 0=left 1=right 2=middle. Unless a widget consumes clicks/wheel first,
 * ui_getkey synthesizes them as Enter / Esc / Up / Down. The super/Windows
 * key is handled by k93ui itself: tap = "home" (frontmost app quits, status
 * 0), hold 5s = "multitasking" (app quits with status 2); inside SpringBoard
 * (K93_NO_SUPER_EXIT set) they arrive as UI_KEY_HOME / UI_KEY_SWITCHER. */
int  ui_mouse_x(void);                   /* cursor px, -1 until first motion */
int  ui_mouse_y(void);
int  ui_mouse_visible(void);
int  ui_mouse_clicked(int btn);          /* consumes: 1 if a press is pending */
int  ui_mouse_scroll(void);              /* consumes: +down / -up lines */

/* widgets; return 0=ok, 1=back/esc */
int  ui_menu(const char *title, const char *const *items, int n, int sel);
void ui_msgbox(const char *title, const char *body);    /* any key closes */
int  ui_confirm(const char *title, const char *body);   /* 1=yes 0=no */
int  ui_input(const char *prompt, char *buf, size_t cap);
int  ui_textview(const char *title, const char *text);

/* --- styled page view (Safari): multi-run lines, colors, images, links ---
 * One run = one text span with its own color/bold/link. A line can also be
 * an image line (img != NULL, runs unused). */
typedef struct {
    const char *text;                /* NUL-terminated, owned by caller pool */
    uint32_t fg;
    int bold;
    int link;                        /* link index activated by Enter, or -1 */
    int font;                        /* k93font id for this run, -1 = default */
    int underline;                   /* bitmask: 1=underline, 2=line-through */
    uint32_t bg;                     /* run background band, 0 = none */
} ui_prun;

typedef struct {
    ui_prun *runs;                   /* text runs (caller-owned) */
    int nruns;                       /* 0 => image line */
    uint32_t bg;                     /* band background, 0 = page white */
    int center, indent, scale;       /* scale 1 or 2 (font is 8x16 per unit) */
    int link;                        /* line's activatable link, or -1 */
    const uint32_t *img;             /* RGBA pixels for image lines or NULL */
    int imgw, imgh;
    int lh;                          /* line height px, 0 = scale default */
} ui_pline;

typedef struct { ui_pline *lines; int n; int cap; int top, sel; } ui_page;

/* Returns >= 0: link index activated with Enter. -1: Esc (close).
 * tick: called each ~250ms idle; returns 1 = page rebuilt (redraw),
 * 2 = close now. key: called on every key before the built-in scroll keys;
 * return nonzero to consume the key (Safari uses it for browser chrome). */
int  ui_pageview(const char *title, ui_page *p,
                 int (*tick)(ui_page *p, void *ctx),
                 int (*key)(int k, ui_page *p, void *ctx), void *ctx);
int  ui_editbox(const char *title, char *buf, size_t cap); /* multiline editor */

#endif
