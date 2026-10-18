/* K93OS YouTube - Invidious front-end (clean-room).
 * The app is powered by a user-specified Invidious instance
 * (/etc/k93/youtube.conf: INSTANCE=https://...). All fetching happens in
 * Safari, which already owns HTTP/TLS/proxy and renders server-side HTML -
 * Invidious pages are plain HTML with links, so browsing/search/trending
 * work in the text browser. Ceiling: no in-app video decode (no H.264/AV1
 * decoder ships on this OS), so playback happens wherever the instance's
 * page links go; instance selection is deliberately user-controlled, not
 * hardwired to any vendor. */
#define _GNU_SOURCE
#include <k93ui.h>
#include <k93util.h>
#include <k93app.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CONF "/etc/k93/youtube.conf"
#define DEFAULT_INSTANCE "https://yewtu.be"

static char instance[256];

static void conf_load(void)
{
    k93_strlcpy(instance, DEFAULT_INSTANCE, sizeof instance);
    size_t len = 0;
    char *c = k93_read_file(CONF, &len);
    if (!c) return;
    for (char *line = strtok(c, "\n"); line; line = strtok(NULL, "\n")) {
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        if (!strcmp(k93_trim(line), "INSTANCE")) {
            char *v = k93_trim(eq + 1);
            if (v[0]) k93_strlcpy(instance, v, sizeof instance);
        }
    }
    free(c);
}

static void conf_save(void)
{
    char line[300];
    snprintf(line, sizeof line, "INSTANCE=%s\n", instance);
    if (k93_write_file(CONF, line, strlen(line), 0) != 0)
        ui_msgbox("YouTube", "Could not write " CONF " (need root?).");
}

static int valid_instance(const char *u)
{
    return !strncmp(u, "https://", 8) || !strncmp(u, "http://", 7);
}

static void strip_slash(char *u)
{
    size_t l = strlen(u);
    while (l > 1 && u[l - 1] == '/') u[--l] = 0;
}

/* open path on the configured instance (path starts with '/') */
static void open_on_instance(const char *path)
{
    char url[512];
    snprintf(url, sizeof url, "%s%s", instance, path);
    k93_app_open_url(url);
}

static void percent_encode(const char *q, char *out, size_t cap)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;
    for (; *q && o + 4 < cap; q++) {
        unsigned char c = (unsigned char)*q;
        if (isalnum((unsigned char)c) || strchr("-_.~", (int)c)) {
            out[o++] = (char)c;
        } else {
            out[o++] = '%';
            out[o++] = hex[c >> 4];
            out[o++] = hex[c & 15];
        }
    }
    out[o] = 0;
}

int main(void)
{
    if (ui_init("YouTube") < 0) return 1;
    conf_load();
    for (;;) {
        char title[320];
        snprintf(title, sizeof title, "YouTube - %.280s", instance);
        const char *items[] = {
            "Browse instance",
            "Trending",
            "Search...",
            "Change instance...",
            "About",
        };
        int sel = ui_menu(title, items, 5, 0);
        if (sel < 0) break;
        if (sel == 0) {
            open_on_instance("/");
        } else if (sel == 1) {
            open_on_instance("/trending");
        } else if (sel == 2) {
            char q[200] = "";
            if (ui_input("Search", q, sizeof q) == 0 && q[0]) {
                char enc[600];
                percent_encode(q, enc, sizeof enc);
                char path[700];
                snprintf(path, sizeof path, "/results?query=%s", enc);
                open_on_instance(path);
            }
        } else if (sel == 3) {
            char url[256];
            snprintf(url, sizeof url, "%.240s", instance);
            if (ui_input("Invidious instance", url, sizeof url) == 0 && url[0]) {
                if (!valid_instance(url)) {
                    ui_msgbox("YouTube",
                              "Instance must start with http:// or https://");
                } else {
                    strip_slash(url);
                    k93_strlcpy(instance, url, sizeof instance);
                    conf_save();
                }
            }
        } else {
            ui_msgbox("YouTube",
                      "Powered by your Invidious instance.\n"
                      "Pages open in Safari (text + links, no JavaScript).\n"
                      "No video decoder ships on this OS, so playback goes\n"
                      "through the instance's linked formats - see the\n"
                      "Known ceilings section in README.md.");
        }
    }
    ui_shutdown();
    return 0;
}
