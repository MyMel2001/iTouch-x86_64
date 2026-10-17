/* K93OS AdSheet - renders local static ads from /var/k93/ads (webview stub). */
#include <k93ui.h>
#include <k93util.h>
#include <dirent.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    char msg[1024];
    if (ui_init("AdSheet") < 0) return 1;
    const char *ad_dir = "/var/k93/ads";
    DIR *d = opendir(ad_dir);
    int n = 0;
    snprintf(msg, sizeof msg, "Local ad creatives in %s:\n\n", ad_dir);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) && n < 20) {
            if (e->d_name[0] == '.') continue;
            strncat(msg, "  ", sizeof msg - strlen(msg) - 1);
            strncat(msg, e->d_name, sizeof msg - strlen(msg) - 1);
            strncat(msg, "\n", sizeof msg - strlen(msg) - 1);
            n++;
        }
        closedir(d);
    }
    if (n == 0)
        snprintf(msg, sizeof msg,
                 "No ads configured in %s.\n\n"
                 "AdSheet only renders creatives from this directory;\n"
                 "it has no network client of its own.", ad_dir);
    ui_msgbox("AdSheet", msg);
    ui_shutdown();
    return 0;
}
