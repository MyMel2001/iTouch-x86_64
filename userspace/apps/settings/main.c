/* Settings.app - K93OS-x86 preferences (clean-room counterpart of the
 * Preferences.app seen in the supplied rootfs).
 * Panes: Network, HTTP Proxy (per user requirement), Certificates, About,
 * Storage, Root Access, Services. Values live in /etc/k93/. */
#define _GNU_SOURCE
#include "k93ui.h"
#include "k93util.h"
#include "k93plist.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <signal.h>
#include <ifaddrs.h>
#include <arpa/inet.h>
#include <net/if.h>

#define CONF "/etc/k93/proxy.conf"

static void conf_load(char *host, size_t hs, int *port, int *enable)
{
    k93_strlcpy(host, "", hs); *port = 0; *enable = 0;
    size_t len = 0;
    char *c = k93_read_file(CONF, &len);
    if (!c) return;
    for (char *line = strtok(c, "\n"); line; line = strtok(NULL, "\n")) {
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char *k = k93_trim(line), *v = k93_trim(eq + 1);
        if (!strcmp(k, "PROXY_HOST")) k93_strlcpy(host, v, hs);
        else if (!strcmp(k, "PROXY_PORT")) *port = atoi(v);
        else if (!strcmp(k, "PROXY_ENABLE")) *enable = atoi(v);
    }
    free(c);
}

static void conf_save(const char *host, int port, int enable)
{
    char buf[512];
    snprintf(buf, sizeof buf,
             "# K93OS HTTP proxy configuration (read by Safari and network tools)\n"
             "PROXY_ENABLE=%d\nPROXY_HOST=%s\nPROXY_PORT=%d\n", enable, host, port);
    k93_mkdir_p("/etc/k93", 0755);
    k93_write_file(CONF, buf, strlen(buf), 0);
}

static void pane_network(void)
{
    char body[2048] = "";
    size_t o = 0;
    struct ifaddrs *ifa = NULL, *i;
    o += (size_t)snprintf(body + o, sizeof body - o, "Interfaces:\n\n");
    if (getifaddrs(&ifa) == 0) {
        for (i = ifa; i && o + 256 < sizeof body; i = i->ifa_next) {
            char addr[INET6_ADDRSTRLEN] = "";
            if (i->ifa_addr && i->ifa_addr->sa_family == AF_INET)
                inet_ntop(AF_INET, &((struct sockaddr_in *)i->ifa_addr)->sin_addr,
                          addr, sizeof addr);
            else if (i->ifa_addr && i->ifa_addr->sa_family == AF_INET6)
                inet_ntop(AF_INET6, &((struct sockaddr_in6 *)i->ifa_addr)->sin6_addr,
                          addr, sizeof addr);
            else
                k93_strlcpy(addr, "(no address)", sizeof addr);
            o += (size_t)snprintf(body + o, sizeof body - o, "  %-10s %-18s flags=0x%x\n",
                                  i->ifa_name, addr, i->ifa_flags);
        }
        freeifaddrs(ifa);
    } else {
        o += (size_t)snprintf(body + o, sizeof body - o, "  getifaddrs failed\n");
    }
    o += (size_t)snprintf(body + o, sizeof body - o,
                          "\nresolv.conf:\n%s", "");
    size_t rl = 0;
    char *r = k93_read_file("/etc/resolv.conf", &rl);
    if (r && o + rl < sizeof body) { memcpy(body + o, r, rl); o += rl; }
    free(r);
    ui_textview("Network", body);
}

static void pane_proxy(void)
{
    char host[128] = "";
    int port = 0, enable = 0;
    conf_load(host, sizeof host, &port, &enable);
    for (;;) {
        char title[64], body[512];
        snprintf(title, sizeof title, "HTTP Proxy");
        snprintf(body, sizeof body,
                 "State : %s\nHost  : %s\nPort  : %d\n\n"
                 "The proxy applies to Safari and any app that reads\n"
                 "%s (standard HTTP_PROXY semantics).\n",
                 enable ? "ON" : "OFF", host[0] ? host : "(none)", port, CONF);
        const char *items[] = {
            "Toggle proxy on/off",
            "Set proxy host",
            "Set proxy port",
            "Clear configuration",
            "Back"
        };
        int sel = ui_menu("HTTP Proxy", items, 5, 0);
        if (sel < 0 || sel == 4) return;
        if (sel == 0) { enable = !enable; conf_save(host, port, enable); }
        else if (sel == 1) {
            if (ui_input("Proxy host", host, sizeof host) == 0) conf_save(host, port, enable);
        } else if (sel == 2) {
            char buf[16] = "";
            snprintf(buf, sizeof buf, "%d", port);
            if (ui_input("Proxy port", buf, sizeof buf) == 0) { port = atoi(buf); conf_save(host, port, enable); }
        } else if (sel == 3) {
            host[0] = 0; port = 0; enable = 0; conf_save(host, port, enable);
        }
        (void)body;
    }
}

static void pane_certs(void)
{
    char body[2048] = "";
    size_t len = 0;
    char *pem = k93_read_file("/etc/ssl/certs/ca-certificates.crt", &len);
    int count = 0;
    if (pem) {
        for (char *p = pem; (p = strstr(p, "BEGIN CERTIFICATE")); p += 20) count++;
    }
    snprintf(body, sizeof body,
             "Trust store: /etc/ssl/certs/ca-certificates.crt\n"
             "Certificates present: %d\n"
             "Size: %zu bytes\n\n"
             "This is the modern Mozilla-derived CA bundle shipped with the OS\n"
             "so that current HTTPS sites validate (the 2011-era roots that\n"
             "shipped with iOS 4.3.4 are long expired).\n\n"
             "Safari uses this file for TLS verification via mbedTLS.\n",
             count, len);
    free(pem);
    ui_textview("Certificates", body);
}

static void pane_about(void)
{
    char err[128];
    k93_plist *v = k93_plist_parse_file("/System/Library/CoreServices/SystemVersion.plist", err, sizeof err);
    const char *prod = v ? k93_plist_dict_str(v, "ProductName", "K93OS-x86") : "K93OS-x86";
    const char *ver = v ? k93_plist_dict_str(v, "ProductVersion", "1.0") : "1.0";
    const char *bld = v ? k93_plist_dict_str(v, "ProductBuildVersion", "unknown") : "unknown";
    char body[1024], kern[128] = "";
    size_t kl = 0;
    char *k = k93_read_file("/proc/version", &kl);
    if (k) { k93_strlcpy(kern, k, sizeof kern); free(k); }
    long up = k93_sysinfo_long("/proc/uptime");
    snprintf(body, sizeof body,
             "Product        : %s (clean-room x86_64 edition)\n"
             "Interface model: %s %s (%s)\n"
             "Kernel         : %s"
             "Uptime         : %ld min\n"
             "Root access    : always available (this is a feature, not a jailbreak)\n",
             prod, prod, ver, bld, kern, up / 60);
    k93_plist_free(v);
    ui_textview("About", body);
}

static void pane_storage(void)
{
    char body[2048] = "";
    size_t o = (size_t)snprintf(body, sizeof body, "Mounts:\n\n");
    FILE *f = fopen("/proc/mounts", "r");
    if (f) {
        char line[256];
        while (fgets(line, sizeof line, f) && o + 128 < sizeof body) {
            char src[64], tgt[64], type[32];
            unsigned long long bsize = 0;
            if (sscanf(line, "%63s %63s %31s %*s %*d %*d", src, tgt, type) == 3) {
                struct statfs_like { int dummy; };
                (void)bsize;
                o += (size_t)snprintf(body + o, sizeof body - o, "  %-22s %-18s %s\n", tgt, type, src);
            }
        }
        fclose(f);
    }
    /* df via statvfs */
    #include <sys/statvfs.h>
    const char *checks[] = { "/", "/var", "/tmp", NULL };
    o += (size_t)snprintf(body + o, sizeof body - o, "\nFree space:\n");
    for (int i = 0; checks[i] && o + 128 < sizeof body; i++) {
        struct statvfs sv;
        if (statvfs(checks[i], &sv) == 0) {
            unsigned long long total = (unsigned long long)sv.f_blocks * sv.f_frsize;
            unsigned long long freeb = (unsigned long long)sv.f_bavail * sv.f_frsize;
            char t[32], f2[32];
            k93_human_size(t, sizeof t, total);
            k93_human_size(f2, sizeof f2, freeb);
            o += (size_t)snprintf(body + o, sizeof body - o, "  %-8s %10s total, %10s free\n",
                                  checks[i], t, f2);
        }
    }
    ui_textview("Storage", body);
}

static void pane_root(void)
{
    ui_msgbox("Root Access",
              "K93OS-x86 grants root access by design.\n"
              "\n"
              "There is no code-signing enforcement, no sandboxing daemon and\n"
              "no immutable system volume. Any app you install from a third\n"
              "party source runs with the privileges you give it.\n"
              "\n"
              "This is not a jailbreak: nothing was bypassed, the OS simply\n"
              "does not implement the restrictions that iOS 4.3.4 had.");
}

#define DISPLAY_CONF "/etc/k93/display.conf"

static void display_conf_save(const char *mode, const char *orient)
{
    char buf[256];
    snprintf(buf, sizeof buf,
             "# K93OS display configuration (read by every graphical app)\n"
             "MODE=%s\nORIENTATION=%s\n", mode, orient);
    k93_mkdir_p("/etc/k93", 0755);
    k93_write_file(DISPLAY_CONF, buf, strlen(buf), 0);
}

static void display_conf_load(char *mode, size_t mlen, char *orient, size_t olen)
{
    k93_strlcpy(mode, "auto", mlen);
    k93_strlcpy(orient, "landscape", olen);
    size_t len = 0;
    char *c = k93_read_file(DISPLAY_CONF, &len);
    if (!c) return;
    for (char *line = strtok(c, "\n"); line; line = strtok(NULL, "\n")) {
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char *k = k93_trim(line), *v = k93_trim(eq + 1);
        if (!strcmp(k, "MODE")) k93_strlcpy(mode, v, mlen);
        else if (!strcmp(k, "ORIENTATION")) k93_strlcpy(orient, v, olen);
    }
    free(c);
}

/* find a process by /proc/<pid>/comm and signal it (restart helpers) */
static int signal_comm(const char *name, int sig)
{
    DIR *d = opendir("/proc");
    struct dirent *e;
    int hit = 0;
    while (d && (e = readdir(d))) {
        if (e->d_name[0] < '1' || e->d_name[0] > '9') continue;
        char p[64], comm[64] = "";
        snprintf(p, sizeof p, "/proc/%s/comm", e->d_name);
        size_t l = 0;
        char *c = k93_read_file(p, &l);
        if (c) { k93_strlcpy(comm, c, sizeof comm); free(c); }
        if (!strncmp(comm, name, strlen(name))) {
            if (kill(atoi(e->d_name), sig) == 0) hit = 1;
        }
    }
    if (d) closedir(d);
    return hit;
}

static void pane_display(void)
{
    char mode[32], orient[32];
    for (;;) {
        display_conf_load(mode, sizeof mode, orient, sizeof orient);
        char modes[32][32];
        int nm = ui_display_modes(modes, 32);
        const char *items[] = {
            "Resolution: current, pick another",
            "Orientation: landscape",
            "Orientation: portrait",
            "Restart SpringBoard (apply now)",
            "Back"
        };
        char title[96];
        snprintf(title, sizeof title, "Display  %s  %s", mode, orient);
        int sel = ui_menu(title, items, 5, 0);
        if (sel < 0 || sel == 4) return;
        if (sel == 0) {
            const char *opts[33];
            int n = 0;
            if (nm == 0) opts[n++] = "auto";
            for (int i = 0; i < nm && n < 32; i++) opts[n] = modes[i], n++;
            opts[n] = "auto"; n++;            /* always allow driver default */
            int pick = ui_menu("Resolution", opts, n, 0);
            if (pick >= 0) { display_conf_save(opts[pick], orient); }
        } else if (sel == 1) {
            display_conf_save(mode, "landscape");
        } else if (sel == 2) {
            display_conf_save(mode, "portrait");
        } else if (sel == 3) {
            int hit = signal_comm("springboard", SIGTERM);
            ui_msgbox("Display", hit
                      ? "SpringBoard restarted; new display settings apply\n"
                        "to every app that launches after it."
                      : "SpringBoard not found running.");
        }
    }
}

static void pane_notesfont(void)
{
    const char *fonts[] = { "sans", "serif", "mono" };
    int nf = 3;
    for (;;) {
        char cur[32] = "sans";
        size_t len = 0;
        char *c = k93_read_file("/etc/k93/notes.conf", &len);
        if (c) {
            for (char *line = strtok(c, "\n"); line; line = strtok(NULL, "\n")) {
                if (!strncmp(line, "FONT=", 5))
                    k93_strlcpy(cur, line + 5, sizeof cur);
            }
            free(c);
        }
        char title[64];
        snprintf(title, sizeof title, "Notes Font  (current: %s)", cur);
        const char *items[5];
        int n = 0;
        for (int i = 0; i < nf; i++) {
            static char buf[3][64];
            snprintf(buf[i], sizeof buf[i], "%s%s",
                     fonts[i], !strcmp(cur, fonts[i]) ? "  (selected)" : "");
            items[n++] = buf[i];
        }
        items[n++] = "Back";
        int sel = ui_menu(title, items, n, 0);
        if (sel < 0 || sel == n - 1) return;
        if (sel >= 0 && sel < nf) {
            k93_mkdir_p("/etc/k93", 0755);
            char b[256];
            snprintf(b, sizeof b, "FONT=%s\n", fonts[sel]);
            k93_write_file("/etc/k93/notes.conf", b, strlen(b), 0);
        }
    }
}

int main(void)
{
    if (ui_init("Settings") < 0) return 1;
    const char *items[] = {
        "Network", "HTTP Proxy", "Display", "Notes Font", "Certificates", "About",
        "Storage", "Root Access", "Restart services (SIGHUP launchd)", "Back"
    };
    for (;;) {
        int sel = ui_menu("Settings", items, 10, 0);
        switch (sel) {
        case 0: pane_network(); break;
        case 1: pane_proxy(); break;
        case 2: pane_display(); break;
        case 3: pane_notesfont(); break;
        case 4: pane_certs(); break;
        case 5: pane_about(); break;
        case 6: pane_storage(); break;
        case 7: pane_root(); break;
        case 8:
            signal_comm("launchd", SIGHUP);
            ui_msgbox("Settings", "SIGHUP delivered to launchd.");
            break;
        default:
            ui_shutdown();
            return 0;
        }
    }
}
