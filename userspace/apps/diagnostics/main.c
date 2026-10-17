/* Diagnostics.app - FieldTest/Reports style system inspection (clean-room).
 * The 4.3.4 rootfs ships FieldTest.app, "iOS Diagnostics.app", ReportCrash and
 * DumpPanic; this app is their x86_64 counterpart: live system facts + logs +
 * a root shell + power controls. */
#define _GNU_SOURCE
#include "k93ui.h"
#include "k93util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/reboot.h>
#include <sys/wait.h>
#include <sys/utsname.h>

static void page_sysinfo(void)
{
    struct utsname u;
    uname(&u);
    char body[2048];
    size_t o = (size_t)snprintf(body, sizeof body,
                                "Kernel   : %s %s %s\nMachine  : %s\n",
                                u.sysname, u.release, u.version, u.machine);
    size_t l = 0;
    char *load = k93_read_file("/proc/loadavg", &l);
    if (load) o += (size_t)snprintf(body + o, sizeof body - o, "Load     : %s", load);
    free(load);
    l = 0;
    char *mem = k93_read_file("/proc/meminfo", &l);
    if (mem) {
        char *nl = strchr(mem, '\n');
        if (nl) *nl = 0;
        o += (size_t)snprintf(body + o, sizeof body - o, "Mem      : %s\n", mem);
        char *m2 = mem + 1;
        char *nl2 = strchr(m2, '\n');
        if (nl2) *nl2 = 0;
        o += (size_t)snprintf(body + o, sizeof body - o, "          %s\n", m2);
    }
    free(mem);
    l = 0;
    char *cpu = k93_read_file("/proc/cpuinfo", &l);
    if (cpu) {
        char *model = strstr(cpu, "model name");
        if (model) {
            char *c = strchr(model, ':');
            if (c) {
                char *e = strchr(c, '\n');
                if (e) *e = 0;
                o += (size_t)snprintf(body + o, sizeof body - o, "CPU      : %s\n", c + 2);
            }
        }
    }
    free(cpu);
    long up = k93_sysinfo_long("/proc/uptime");
    o += (size_t)snprintf(body + o, sizeof body - o, "Uptime   : %ldm%lds\n", up / 60, up % 60);
    ui_textview("System Info", body);
}

static void page_procs(void)
{
    char body[4096] = "";
    size_t o = 0;
    DIR *d = opendir("/proc");
    struct dirent *e;
    while (d && (e = readdir(d)) && o + 256 < sizeof body) {
        if (e->d_name[0] < '1' || e->d_name[0] > '9') continue;
        char p[64], comm[64] = "";
        snprintf(p, sizeof p, "/proc/%s/comm", e->d_name);
        size_t l = 0;
        char *c = k93_read_file(p, &l);
        if (c) { k93_strlcpy(comm, c, sizeof comm); free(c); }
        char *nl = strchr(comm, '\n');
        if (nl) *nl = 0;
        o += (size_t)snprintf(body + o, sizeof body - o, "%6s  %s\n", e->d_name, comm);
    }
    if (d) closedir(d);
    ui_textview("Processes", body);
}

static void page_blocks(void)
{
    char body[3072] = "";
    size_t o = (size_t)snprintf(body, sizeof body, "Block devices:\n\n");
    DIR *d = opendir("/sys/block");
    struct dirent *e;
    while (d && (e = readdir(d)) && o + 256 < sizeof body) {
        if (e->d_name[0] == '.') continue;
        char p[256], mp[256];
        snprintf(p, sizeof p, "/sys/block/%s/size", e->d_name);
        long long sz = k93_sysinfo_long(p);
        char model[64] = "";
        snprintf(mp, sizeof mp, "/sys/block/%s/device/model", e->d_name);
        size_t l = 0;
        char *m = k93_read_file(mp, &l);
        if (m) { k93_strlcpy(model, k93_trim(m), sizeof model); free(m); }
        char h[32];
        k93_human_size(h, sizeof h, (unsigned long long)sz * 512ULL);
        o += (size_t)snprintf(body + o, sizeof body - o, "  %-8s %8s  %.40s\n", e->d_name, h, model);
    }
    if (d) closedir(d);
    o += (size_t)snprintf(body + o, sizeof body - o, "\nMounts:\n");
    FILE *f = fopen("/proc/mounts", "r");
    if (f) {
        char line[256];
        while (fgets(line, sizeof line, f) && o + 128 < sizeof body) {
            char src[64], tgt[64], type[32];
            if (sscanf(line, "%63s %63s %31s", src, tgt, type) == 3)
                o += (size_t)snprintf(body + o, sizeof body - o, "  %-18s %-10s %s\n", tgt, type, src);
        }
        fclose(f);
    }
    ui_textview("Storage", body);
}

static void page_logs(void)
{
    size_t l = 0;
    char *log = k93_read_file("/var/log/k93.log", &l);
    if (!log) { ui_textview("Logs", "No log yet."); return; }
    /* show the tail */
    size_t start = l > 4000 ? l - 4000 : 0;
    ui_textview("System log (tail)", log + start);
    free(log);
}

static void page_smc(void)
{
    /* clean-room stand-in for the SMC: ACPI + sysfs power/thermal facts */
    char body[2048] = "";
    size_t o = (size_t)snprintf(body, sizeof body,
        "SMC replacement layer (clean-room):\n\n"
        "The original Apple SMC is a firmware coprocessor that owns battery,\n"
        "thermal and power-button state, and enforces some DRM checks. On PC\n"
        "hardware the equivalent services come from ACPI and the kernel.\n\n");
    DIR *d = opendir("/sys/class/power_supply");
    struct dirent *e;
    int any = 0;
    while (d && (e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        any = 1;
        char p[256], cap[32] = "";
        snprintf(p, sizeof p, "/sys/class/power_supply/%s/type", e->d_name);
        size_t l = 0;
        char *t = k93_read_file(p, &l);
        snprintf(p, sizeof p, "/sys/class/power_supply/%s/capacity", e->d_name);
        l = 0;
        char *c = k93_read_file(p, &l);
        if (c) { k93_strlcpy(cap, k93_trim(c), sizeof cap); free(c); }
        o += (size_t)snprintf(body + o, sizeof body - o, "  %-10s type=%s capacity=%s\n",
                              e->d_name, t ? k93_trim(t) : "?", cap[0] ? cap : "-");
        free(t);
    }
    if (d) closedir(d);
    if (!any) o += (size_t)snprintf(body + o, sizeof body - o, "  (no power supply class devices)\n");
    o += (size_t)snprintf(body + o, sizeof body - o, "\nACPI DSDT present: %s\n",
                          access("/sys/firmware/acpi/tables/DSDT", R_OK) == 0 ? "yes" : "no");
    o += (size_t)snprintf(body + o, sizeof body - o, "EFI vars present: %s\n",
                          access("/sys/firmware/efi/efivars", F_OK) == 0 ? "yes" : "no");
    ui_textview("SMC / Power", body);
}

int main(void)
{
    if (ui_init("Diagnostics") < 0) return 1;
    for (;;) {
        const char *items[] = {
            "System info", "Processes", "Storage and mounts", "System log",
            "SMC / power / ACPI", "Root shell", "Rescan devices (mdev -s)",
            "Sync filesystems", "Reboot", "Power off", "Back"
        };
        int sel = ui_menu("Diagnostics", items, 11, 0);
        switch (sel) {
        case 0: page_sysinfo(); break;
        case 1: page_procs(); break;
        case 2: page_blocks(); break;
        case 3: page_logs(); break;
        case 4: page_smc(); break;
        case 5:
            ui_shutdown();
            {
                pid_t p = fork();
                if (p == 0) { execl("/bin/busybox", "busybox", "sh", "-l", (char *)NULL); _exit(127); }
                int st = 0;
                waitpid(p, &st, 0);
            }
            ui_init("Diagnostics");
            break;
        case 6: {
            pid_t p = fork();
            if (p == 0) { execl("/bin/busybox", "busybox", "mdev", "-s", (char *)NULL); _exit(127); }
            int st = 0; waitpid(p, &st, 0);
            ui_msgbox("Diagnostics", "mdev -s completed (device nodes refreshed).");
            break;
        }
        case 7: sync(); ui_msgbox("Diagnostics", "sync() done."); break;
        case 8:
            if (ui_confirm("Reboot", "Reboot the machine now?")) {
                ui_shutdown();
                sync();
                reboot(RB_AUTOBOOT);
            }
            break;
        case 9:
            if (ui_confirm("Power off", "Power off the machine now?")) {
                ui_shutdown();
                sync();
                reboot(RB_POWER_OFF);
            }
            break;
        default:
            ui_shutdown();
            return 0;
        }
    }
}
