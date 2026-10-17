/* Contacts.app - clean-room counterpart of Contacts~ipad.app (4.3.4 rootfs).
 * Storage: one plain-text card per contact in /var/mobile/Library/Contacts. */
#define _GNU_SOURCE
#include "k93ui.h"
#include "k93util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>

#define DIRN "/var/mobile/Library/Contacts"

static void sanitize(char *s)
{
    for (; *s; s++) if (*s == '/' || *s == '\n' || *s == '\r') *s = '_';
}

int main(void)
{
    if (ui_init("Contacts") < 0) return 1;
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
            if (l < 5 || strcmp(e->d_name + l - 4, ".card")) continue;
            k93_strlcpy(names[n], e->d_name, sizeof names[n]);
            names[n][l - 5] = 0;
            items[n] = names[n];
            n++;
        }
        if (d) closedir(d);
        items[n] = "+ New contact";
        items[n + 1] = "Back";
        if (msg[0]) { ui_statusmsg(msg); msg[0] = 0; }
        int sel = ui_menu("Contacts", items, n + 2, 0);
        if (sel < 0 || sel == n + 1) break;
        if (sel == n) {
            char name[96] = "";
            if (ui_input("Name", name, sizeof name) == 0 && name[0]) {
                sanitize(name);
                char phone[64] = "";
                ui_input("Phone", phone, sizeof phone);
                char email[96] = "";
                ui_input("Email", email, sizeof email);
                char note[192] = "";
                ui_input("Note", note, sizeof note);
                char path[256], body[512];
                snprintf(path, sizeof path, "%s/%s.card", DIRN, name);
                snprintf(body, sizeof body, "Name: %s\nPhone: %s\nEmail: %s\nNote: %s\n",
                         name, phone, email, note);
                k93_write_file(path, body, strlen(body), 0);
                snprintf(msg, sizeof msg, "saved %s", name);
            }
            continue;
        }
        char path[256], body[512] = "";
        snprintf(path, sizeof path, "%s/%s.card", DIRN, names[sel]);
        size_t l = 0;
        char *t = k93_read_file(path, &l);
        if (t) { k93_strlcpy(body, t, sizeof body); free(t); }
        const char *acts[] = { "View card", "Edit note", "Delete", "Cancel" };
        int a = ui_menu(names[sel], acts, 4, 0);
        if (a == 0) ui_textview(names[sel], body);
        else if (a == 1) {
            ui_editbox(names[sel], body, sizeof body);
            k93_write_file(path, body, strlen(body), 0);
        } else if (a == 2 && ui_confirm("Delete", "Delete this contact?")) {
            unlink(path);
            snprintf(msg, sizeof msg, "deleted %s", names[sel]);
        }
    }
    ui_shutdown();
    return 0;
}
