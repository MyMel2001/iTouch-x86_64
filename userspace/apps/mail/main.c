/* Mail.app - clean-room counterpart of iOS 4.0 MobileMail with multiple
 * inboxes. Accounts/inboxes are directories under /var/mobile/Library/Mail,
 * messages are plain .eml files. Ceiling: no SMTP/IMAP client ships on this
 * OS (clean-room scope), so this is a local mailbox browser/composer, not a
 * network mail client. */
#define _GNU_SOURCE
#include "k93ui.h"
#include "k93util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <time.h>

#define MAILDIR "/var/mobile/Library/Mail"

static void sanitize(char *s)
{
    for (; *s; s++) if (*s == '/' || *s == '\n' || *s == '\r') *s = '_';
}

static int list_dirs(char out[][128], int max)
{
    int n = 0;
    DIR *d = opendir(MAILDIR);
    struct dirent *e;
    while (d && (e = readdir(d)) && n < max) {
        if (e->d_name[0] == '.') continue;
        char p[256];
        snprintf(p, sizeof p, "%s/%s", MAILDIR, e->d_name);
        struct stat st;
        if (stat(p, &st) == 0 && S_ISDIR(st.st_mode)) {
            k93_strlcpy(out[n], e->d_name, 128);
            n++;
        }
    }
    if (d) closedir(d);
    return n;
}

static int list_msgs(const char *box, char names[64][128], char subs[64][160])
{
    int n = 0;
    DIR *d = opendir(box);
    struct dirent *e;
    while (d && (e = readdir(d)) && n < 64) {
        size_t l = strlen(e->d_name);
        if (l < 5 || strcmp(e->d_name + l - 4, ".eml")) continue;
        k93_strlcpy(names[n], e->d_name, 128);
        k93_strlcpy(subs[n], "(no subject)", 160);
        char path[512];
        snprintf(path, sizeof path, "%s/%s", box, e->d_name);
        size_t fl = 0;
        char *t = k93_read_file(path, &fl);
        if (t) {
            char *s = strcasestr(t, "Subject:");
            if (s) {
                s += 8;
                while (*s == ' ') s++;
                char *e2 = strchr(s, '\n');
                size_t sl = e2 ? (size_t)(e2 - s) : strlen(s);
                if (sl > 159) sl = 159;
                memcpy(subs[n], s, sl);
                subs[n][sl] = 0;
                while (sl && (subs[n][sl - 1] == '\r' || subs[n][sl - 1] == ' ')) subs[n][--sl] = 0;
            }
            free(t);
        }
        n++;
    }
    if (d) closedir(d);
    return n;
}

static void msg_path(const char *box, const char *name, char *out, size_t cap)
{
    snprintf(out, cap, "%s/%s", box, name);
}

static void compose_ui(const char *box)
{
    char to[160] = "", subj[160] = "", body[4000] = "";
    if (ui_input("To", to, sizeof to) != 0) return;
    if (ui_input("Subject", subj, sizeof subj) != 0) return;
    ui_editbox("Message", body, sizeof body);
    if (!subj[0] && !body[0]) return;
    char sent[512];
    snprintf(sent, sizeof sent, "%s/Sent", box);
    k93_mkdir_p(sent, 0777);
    int idx = 0;
    char path[600];
    for (int i = 1; i < 100000; i++) {
        snprintf(path, sizeof path, "%s/msg-%05d.eml", sent, i);
        if (access(path, F_OK) != 0) { idx = i; break; }
    }
    char hdr[1024];
    time_t t = time(NULL);
    char date[64];
    ctime_r(&t, date);
    size_t dl = strlen(date);
    while (dl && (date[dl - 1] == '\n' || date[dl - 1] == '\r')) date[--dl] = 0;
    snprintf(hdr, sizeof hdr,
             "From: mobile@k93.local\r\nTo: %s\r\nSubject: %s\r\nDate: %s\r\n\r\n",
             to, subj, date);
    k93_write_file(path, hdr, strlen(hdr), 0);
    k93_write_file(path, body, strlen(body), 1);
    ui_statusmsg("saved to Sent");
}

static void box_ui(const char *box, const char *title)
{
    for (;;) {
        char names[64][128], subs[64][160];
        const char *items[66];
        int n = list_msgs(box, names, subs);
        for (int i = 0; i < n; i++) items[i] = subs[i];
        items[n++] = "+ Compose";
        items[n++] = "Back";
        int sel = ui_menu(title, items, n, 0);
        if (sel < 0 || sel == n - 1) return;
        if (sel == n - 2) { compose_ui(box); continue; }
        char path[600];
        msg_path(box, names[sel], path, sizeof path);
        size_t l = 0;
        char *t = k93_read_file(path, &l);
        if (!t) { ui_msgbox("Mail", "Cannot read message"); continue; }
        ui_textview(subs[sel][0] ? subs[sel] : names[sel], t);
        int k = ui_getkey(600);
        if (k == UI_KEY_CHAR_BASE + 'd' || k == UI_KEY_CHAR_BASE + 'D') {
            if (ui_confirm("Mail", "Delete this message?")) {
                unlink(path);
                ui_statusmsg("deleted");
            }
        }
        free(t);
    }
}

static void account_ui(const char *acct)
{
    char box[512];
    snprintf(box, sizeof box, "%s/%s", MAILDIR, acct);
    k93_mkdir_p(box, 0777);
    for (;;) {
        const char *items[] = { "Inbox", "Sent", "Delete account", "Back" };
        int sel = ui_menu(acct, items, 4, 0);
        if (sel < 0 || sel == 3) return;
        if (sel == 0) {
            char p[600];
            snprintf(p, sizeof p, "%s/INBOX", box);
            k93_mkdir_p(p, 0777);
            box_ui(p, "Inbox");
        } else if (sel == 1) {
            char p[600];
            snprintf(p, sizeof p, "%s/Sent", box);
            k93_mkdir_p(p, 0777);
            box_ui(p, "Sent");
        } else {
            char q[300];
            snprintf(q, sizeof q, "Delete account '%s' and all its mail?", acct);
            if (ui_confirm("Mail", q)) { k93_rm_tree(box); return; }
        }
    }
}

int main(void)
{
    if (ui_init("Mail") < 0) return 1;
    k93_mkdir_p(MAILDIR, 0777);
    for (;;) {
        char accts[64][128];
        const char *items[66];
        int n = list_dirs(accts, 64);
        for (int i = 0; i < n; i++) items[i] = accts[i];
        items[n++] = "+ New account (inbox)";
        items[n++] = "Back";
        int sel = ui_menu("Mail", items, n, 0);
        if (sel < 0 || sel == n - 1) break;
        if (sel == n - 2) {
            char name[128] = "Personal";
            if (ui_input("Account / inbox name", name, sizeof name) == 0 && name[0]) {
                sanitize(name);
                account_ui(name);
            }
            continue;
        }
        account_ui(accts[sel]);
    }
    ui_shutdown();
    return 0;
}
