/* k93smc - hardware status reader in the style of Apple SMC key queries,
 * backed by Linux sysfs (power supply, thermal zones, CPU). Clean-room:
 * key *names* follow the familiar SMC convention; all values come from
 * the host kernel. Used by Diagnostics app. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <dirent.h>
#include <k93util.h>

static void key_val(const char *key, const char *fmt, ...)
{
    char v[128];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(v, sizeof(v), fmt, ap);
    va_end(ap);
    printf("%-6s %s\n", key, v);
}

static void dump_battery(void)
{
    DIR *d = opendir("/sys/class/power_supply");
    if (!d) { key_val("BSrc", "none (AC-only machine)"); return; }
    struct dirent *e;
    int found = 0;
    while ((e = readdir(d))) {
        char p[256], buf[64];
        if (e->d_name[0] == '.') continue;
        snprintf(p, sizeof(p), "/sys/class/power_supply/%s/type", e->d_name);
        size_t n = 0; char *t = k93_read_file(p, &n);
        if (!t) continue;
        if (!strstr(t, "Battery")) { free(t); continue; }
        free(t); found = 1;
        long cap = -1, volt = -1, cur = 0;
        snprintf(p, sizeof(p), "/sys/class/power_supply/%s/capacity", e->d_name);
        cap = k93_sysinfo_long(p);
        snprintf(p, sizeof(p), "/sys/class/power_supply/%s/voltage_now", e->d_name);
        volt = k93_sysinfo_long(p);
        snprintf(p, sizeof(p), "/sys/class/power_supply/%s/current_now", e->d_name);
        cur = k93_sysinfo_long(p);
        key_val("BNum", "%s", e->d_name);
        key_val("BChg", "%ld%%", cap);
        if (volt > 0) key_val("BVlt", "%ld mV", volt / 1000);
        if (cur) key_val("BAmp", "%ld mA", cur / 1000);
        (void)buf;
    }
    closedir(d);
    if (!found) key_val("BSrc", "none (AC-only machine)");
}

static void dump_thermal(void)
{
    DIR *d = opendir("/sys/class/thermal");
    if (!d) { key_val("TC0F", "n/a"); return; }
    struct dirent *e;
    int zone = 0;
    while ((e = readdir(d))) {
        if (strncmp(e->d_name, "thermal_zone", 12)) continue;
        char p[256];
        snprintf(p, sizeof(p), "/sys/class/thermal/%s/temp", e->d_name);
        long t = k93_sysinfo_long(p);
        if (t < 0) continue;
        if (t > 1000) t /= 1000; /* millideg -> deg */
        char key[8];
        snprintf(key, sizeof(key), "TC%02d", zone++);
        key_val(key, "%ld C (%s)", t, e->d_name);
        if (zone >= 6) break;
    }
    closedir(d);
}

static void dump_cpu(void)
{
    FILE *f = fopen("/proc/cpuinfo", "r");
    int procs = 0, mhz = 0;
    char line[256];
    if (f) {
        while (fgets(line, sizeof(line), f)) {
            if (!strncmp(line, "processor", 9)) procs++;
            if (!strncmp(line, "cpu MHz", 7)) sscanf(line, "cpu MHz : %d", &mhz);
        }
        fclose(f);
    }
    key_val("NPRO", "%d", procs);
    if (mhz) key_val("FREQ", "%d MHz", mhz);
    size_t ln = 0;
    char *model = k93_read_file("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor", &ln);
    if (model) { char *s = k93_trim(model); key_val("GOVN", "%s", s); free(model); }
}

int main(void)
{
    printf("K93SMC - hardware status (Linux sysfs backend)\n");
    dump_cpu();
    dump_battery();
    dump_thermal();
    return 0;
}
