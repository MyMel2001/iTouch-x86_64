/* Calendar.app - clean-room counterpart of MobileCal.app (4.3.4 rootfs).
 * Month grid + events stored as "YYYY-MM-DD|text" lines. */
#define _GNU_SOURCE
#include "k93ui.h"
#include "k93util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

#define EVENTS "/var/mobile/Library/Calendar/events.txt"

static int dim(int y, int m)
{
    static const int d[] = { 31,28,31,30,31,30,31,31,30,31,30,31 };
    if (m == 1 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)) return 29;
    return d[m];
}

static void draw_month(int year, int month, int sel)
{
    ui_clear();
    ui_statusbar("Calendar");
    static const char *mn[] = { "January","February","March","April","May","June",
                                "July","August","September","October","November","December" };
    ui_printf(2, 1, "%s %d", mn[month], year);
    ui_puts(2, 2, "Mo Tu We Th Fr Sa Su");
    struct tm tm;
    memset(&tm, 0, sizeof tm);
    tm.tm_year = year - 1900;
    tm.tm_mday = 1;
    tm.tm_mon = month;
    mktime(&tm);
    int dow = (tm.tm_wday + 6) % 7;                 /* Monday-based */
    int days = dim(year, month);
    int row = 3, col = dow;
    for (int d = 1; d <= days; d++) {
        char cell[8];
        snprintf(cell, sizeof cell, "%2d", d);
        int x = 2 + col * 3, y = row;
        if (d == sel) ui_puts_inv(x, y, cell);
        else ui_printf(x, y, "%s", cell);
        col++;
        if (col == 7) { col = 0; row++; }
    }
    ui_statusmsg("Left/Right month  Up/Down day  Enter events  Esc back");
    ui_flush();
}

/* iOS 4.2: import .ics calendar files directly (VEVENT -> local events) */
static void import_ics(const char *path)
{
    size_t l = 0;
    char *t = k93_read_file(path, &l);
    if (!t) { char m[256]; snprintf(m, sizeof m, "cannot open %.80s", path);
              ui_msgbox("Calendar", m); return; }
    FILE *out = fopen(EVENTS, "a");
    if (!out) { free(t); return; }
    int n = 0;
    char *p = t;
    while (*p) {
        char *be = strstr(p, "BEGIN:VEVENT");
        if (!be) break;
        char *ee = strstr(be, "END:VEVENT");
        if (!ee) break;
        char *body = be + strlen("BEGIN:VEVENT");
        char ev[160] = "";
        char *s = strstr(body, "SUMMARY:");
        if (s) {
            char *e2 = strchr(s, '\n');
            size_t sl = e2 ? (size_t)(e2 - s) - 9 : strlen(s + 9);
            if (sl > sizeof ev - 1) sl = sizeof ev - 1;
            memcpy(ev, s + 9, sl); ev[sl] = 0;
            while (sl && (ev[sl - 1] == '\r' || ev[sl - 1] == ' ')) ev[--sl] = 0;
        }
        char ymd[9] = "";
        char *d = strstr(body, "DTSTART");
        if (d) {
            char *colon = strchr(d, ':');
            if (colon) {
                char *q = colon + 1;
                for (int i = 0; i < 8 && q[i] >= '0' && q[i] <= '9'; i++)
                    ymd[i] = q[i];
            }
        }
        if (ymd[0]) {
            char key[16];
            snprintf(key, sizeof key, "%c%c%c%c-%c%c-%c%c",
                     ymd[0], ymd[1], ymd[2], ymd[3],
                     ymd[4], ymd[5], ymd[6], ymd[7]);
            fprintf(out, "%s|%s\n", key, ev);
            n++;
        }
        p = ee + strlen("END:VEVENT");
    }
    fclose(out);
    free(t);
    char m[96];
    snprintf(m, sizeof m, "Imported %d event(s).", n);
    ui_msgbox("Calendar", m);
}

int main(void)
{
    if (ui_init("Calendar") < 0) return 1;
    k93_mkdir_p("/var/mobile/Library/Calendar", 0777);
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    int year = tm.tm_year + 1900, month = tm.tm_mon, sel = tm.tm_mday;

    for (;;) {
        draw_month(year, month, sel);
        int k = ui_getkey(250);
        if (k == UI_KEY_ESC || k == UI_KEY_CTRL_C) break;
        else if (k == UI_KEY_LEFT) { if (--month < 0) { month = 11; year--; } }
        else if (k == UI_KEY_RIGHT) { if (++month > 11) { month = 0; year++; } }
        else if (k == UI_KEY_UP) { if (sel > 1) sel--; }
        else if (k == UI_KEY_DOWN) { if (sel < dim(year, month)) sel++; }
        else if (k == UI_KEY_ENTER) {
            /* show events for this date, allow adding */
            char key[16];
            snprintf(key, sizeof key, "%04d-%02d-%02d", year, month + 1, sel);
            char body[4096] = "";
            size_t l = 0;
            char *all = k93_read_file(EVENTS, &l);
            if (all) {
                size_t o = 0;
                for (char *line = strtok(all, "\n"); line; line = strtok(NULL, "\n")) {
                    if (!strncmp(line, key, 10) && line[10] == '|') {
                        o += (size_t)snprintf(body + o, sizeof body - o, "  %s\n", line + 11);
                    }
                }
                free(all);
            }
            char title[64];
            snprintf(title, sizeof title, "Events %s", key);
            if (!body[0]) k93_strlcpy(body, "  (no events)\n", sizeof body);
            char wrapped[4600];
            snprintf(wrapped, sizeof wrapped,
                     "%s\n[A]dd  [D]elete last  [I]mport .ics  [Esc]close", body);
            ui_textview(title, wrapped);
            int k2 = ui_getkey(600);
            if (k2 == UI_KEY_CHAR_BASE + 'a' || k2 == UI_KEY_CHAR_BASE + 'A') {
                char ev[160] = "";
                if (ui_input("New event text", ev, sizeof ev) == 0) {
                    FILE *f = fopen(EVENTS, "a");
                    if (f) { fprintf(f, "%s|%s\n", key, ev); fclose(f); }
                }
            } else if (k2 == UI_KEY_CHAR_BASE + 'i' ||
                       k2 == UI_KEY_CHAR_BASE + 'I') {
                char path[512];
                if (ui_input(".ics file path", path, sizeof path) == 0)
                    import_ics(path);
            } else if (k2 == UI_KEY_CHAR_BASE + 'd' || k2 == UI_KEY_CHAR_BASE + 'D') {
                size_t l2 = 0;
                char *all2 = k93_read_file(EVENTS, &l2);
                if (all2) {
                    char *out = k93_xmalloc(l2 + 1);
                    size_t o = 0;
                    for (char *line = strtok(all2, "\n"); line; line = strtok(NULL, "\n")) {
                        if (strncmp(line, key, 10) == 0 && line[10] == '|') continue;
                        o += (size_t)snprintf(out + o, l2 + 1 - o, "%s\n", line);
                    }
                    k93_write_file(EVENTS, out, o, 0);
                    free(out);
                    free(all2);
                }
            }
        }
    }
    ui_shutdown();
    return 0;
}
