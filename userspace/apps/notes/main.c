/* Notes.app - clean-room counterpart of MobileNotes.app from the 4.3.4 rootfs.
 * Notes are plain text files in /var/mobile/Library/Notes. */
#define _GNU_SOURCE
#include "k93ui.h"
#include "k93util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <time.h>

#define DIRN "/var/mobile/Library/Notes"

static void sanitize(char *s)
{
    for (; *s; s++) if (*s == '/' || *s == '\n' || *s == '\r') *s = '_';
}

int main(void)
{
    if (ui_init("Notes") < 0) return 1;
    /* iOS 4.2: Notes font (Settings > Notes Font). sans = Helvetica stand-in,
     * serif = Chalkboard stand-in, mono = Marker Felt stand-in. */
    {
        size_t len = 0;
        char *c = k93_read_file("/etc/k93/notes.conf", &len);
        if (c) {
            for (char *line = strtok(c, "\n"); line; line = strtok(NULL, "\n"))
                if (!strncmp(line, "FONT=", 5)) ui_set_font(line + 5);
            free(c);
        }
    }
    k93_mkdir_p(DIRN, 0777);
    char msg[128] = "";
    for (;;) {
        char names[64][128];
        const char *items[70];
        int n = 0;
        DIR *d = opendir(DIRN);
        struct dirent *e;
        while (d && (e = readdir(d)) && n < 60) {
            size_t l = strlen(e->d_name);
            if (l < 5 || strcmp(e->d_name + l - 4, ".txt")) continue;
            k93_strlcpy(names[n], e->d_name, sizeof names[n]);
            names[n][l - 4] = 0;
            items[n] = names[n];
            n++;
        }
        if (d) closedir(d);
        items[n] = "+ New note";
        items[n + 1] = "Back";
        if (msg[0]) { ui_statusmsg(msg); msg[0] = 0; }
        int sel = ui_menu("Notes", items, n + 2, 0);
        if (sel < 0 || sel == n + 1) break;
        if (sel == n) {
            char title[96] = "New Note";
            if (ui_input("Note title", title, sizeof title) == 0) {
                sanitize(title);
                char path[256];
                snprintf(path, sizeof path, "%s/%s.txt", DIRN, title);
                char body[16000] = "";
                ui_editbox(title, body, sizeof body);
                k93_write_file(path, body, strlen(body), 0);
                snprintf(msg, sizeof msg, "saved %s", title);
            }
            continue;
        }
        /* open existing */
        char path[256], body[16000] = "";
        snprintf(path, sizeof path, "%s/%s.txt", DIRN, names[sel]);
        size_t l = 0;
        char *t = k93_read_file(path, &l);
        if (t) { k93_strlcpy(body, t, sizeof body); free(t); }
        const char *acts[] = { "Read", "Edit", "Delete", "Cancel" };
        int a = ui_menu(names[sel], acts, 4, 0);
        if (a == 0) ui_textview(names[sel], body);
        else if (a == 1) {
            ui_editbox(names[sel], body, sizeof body);
            k93_write_file(path, body, strlen(body), 0);
            snprintf(msg, sizeof msg, "saved %s", names[sel]);
        } else if (a == 2) {
            if (ui_confirm("Delete", "Delete this note?")) {
                unlink(path);
                snprintf(msg, sizeof msg, "deleted %s", names[sel]);
            }
        }
    }
    ui_shutdown();
    return 0;
}
