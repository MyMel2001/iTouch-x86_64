/* launchd.c - K93OS-x86 service supervisor (clean-room).
 * Modelled on the behaviour observed in the supplied iOS 4.3.4 rootfs:
 * /sbin/launchd is PID 1's child that reads XML plist job definitions
 * (Label, ProgramArguments, RunAtLoad, KeepAlive, EnvironmentVariables) from
 * /System/Library/LaunchDaemons and keeps jobs alive. This implementation is
 * original POSIX code reading the same plist format via our own parser. */
#define _GNU_SOURCE
#include "k93util.h"
#include "k93plist.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>

#define MAXJOBS 64
#define LAUNCHDIR "/etc/launchd"

struct job {
    char label[128];
    char plist[256];
    char *argv[32];
    int argc;
    int keepalive;
    int runatload;
    pid_t pid;
    time_t last_start;
    int restarts;
    int enabled;
};

static struct job J[MAXJOBS];
static int njobs = 0;
static volatile sig_atomic_t stop_all = 0;
static volatile sig_atomic_t reload_req = 0;

static void on_term(int s) { (void)s; stop_all = 1; }
static void on_hup(int s) { (void)s; reload_req = 1; }
static void on_chld(int s) { (void)s; }

static void load_job(const char *path)
{
    if (njobs >= MAXJOBS) return;
    char err[160];
    k93_plist *p = k93_plist_parse_file(path, err, sizeof err);
    if (!p) {
        k93_log("launchd", "cannot parse %s: %s", path, err);
        return;
    }
    struct job *j = &J[njobs];
    memset(j, 0, sizeof *j);
    k93_strlcpy(j->label, k93_plist_dict_str(p, "Label", "unknown"), sizeof j->label);
    k93_strlcpy(j->plist, path, sizeof j->plist);
    j->runatload = k93_plist_dict_int(p, "RunAtLoad", 1) ? 1 : 0;
    j->keepalive = k93_plist_dict_int(p, "KeepAlive", 0) ? 1 : 0;
    const k93_plist *args = k93_plist_dict_get(p, "ProgramArguments");
    int n = k93_plist_array_count(args);
    for (int i = 0; i < n && j->argc < 30; i++) {
        const char *s = k93_plist_string(k93_plist_array_at(args, i));
        if (s) j->argv[j->argc++] = strdup(s);
    }
    if (j->argc == 0) {
        const char *prog = k93_plist_dict_str(p, "Program", NULL);
        if (prog) j->argv[j->argc++] = strdup(prog);
    }
    j->argv[j->argc] = NULL;
    j->enabled = 1;
    k93_plist_free(p);
    if (j->argc > 0) {
        njobs++;
        k93_log("launchd", "loaded job %s (%s)", j->label, path);
    }
}

static void load_jobs(void)
{
    DIR *d = opendir(LAUNCHDIR);
    if (!d) {
        k93_log("launchd", "no %s directory", LAUNCHDIR);
        return;
    }
    struct dirent *e;
    while ((e = readdir(d))) {
        size_t l = strlen(e->d_name);
        if (l < 7 || strcmp(e->d_name + l - 6, ".plist")) continue;
        char p[512];
        snprintf(p, sizeof p, "%s/%s", LAUNCHDIR, e->d_name);
        load_job(p);
    }
    closedir(d);
}

static void spawn(struct job *j)
{
    time_t now = time(NULL);
    if (j->last_start && now - j->last_start < 1 && j->restarts > 3) {
        /* crash loop protection: back off */
        sleep(2);
    }
    j->last_start = now;
    pid_t pid = fork();
    if (pid < 0) return;
    if (pid == 0) {
        setsid();
        int fd = open("/dev/null", O_RDWR);
        if (fd >= 0) {
            dup2(fd, 0); dup2(fd, 1); dup2(fd, 2);
            if (fd > 2) close(fd);
        }
        execvp(j->argv[0], j->argv);
        dprintf(2, "launchd: exec %s failed: %s\n", j->argv[0], strerror(errno));
        _exit(127);
    }
    j->pid = pid;
    j->restarts++;
    k93_log("launchd", "started %s (pid %d)", j->label, (int)pid);
}

static void stop_job(struct job *j)
{
    if (j->pid > 0) {
        kill(-j->pid, SIGTERM);
        kill(j->pid, SIGTERM);
        int st;
        for (int i = 0; i < 20; i++) {
            if (waitpid(j->pid, &st, WNOHANG) == j->pid) { j->pid = 0; return; }
            usleep(100000);
        }
        kill(-j->pid, SIGKILL);
        kill(j->pid, SIGKILL);
        waitpid(j->pid, &st, WNOHANG);
        j->pid = 0;
    }
}

int main(void)
{
    k93_log("launchd", "starting, supervising %s", LAUNCHDIR);
    signal(SIGTERM, on_term);
    signal(SIGHUP, on_hup);
    signal(SIGCHLD, on_chld);
    signal(SIGINT, on_term);

    load_jobs();
    for (int i = 0; i < njobs; i++)
        if (J[i].runatload) spawn(&J[i]);

    while (!stop_all) {
        int st;
        pid_t p;
        while ((p = waitpid(-1, &st, WNOHANG)) > 0) {
            for (int i = 0; i < njobs; i++) {
                if (J[i].pid == p) {
                    k93_log("launchd", "job %s (pid %d) exited status %d",
                            J[i].label, (int)p, WIFEXITED(st) ? WEXITSTATUS(st) : -1);
                    J[i].pid = 0;
                }
            }
        }
        if (reload_req) {
            reload_req = 0;
            k93_log("launchd", "SIGHUP: reloading job definitions");
            for (int i = 0; i < njobs; i++) { stop_job(&J[i]); free(J[i].argv[0]); }
            njobs = 0;
            load_jobs();
            for (int i = 0; i < njobs; i++) if (J[i].runatload) spawn(&J[i]);
        }
        for (int i = 0; i < njobs; i++) {
            if (J[i].enabled && J[i].keepalive && J[i].pid == 0) {
                spawn(&J[i]);
            }
        }
        usleep(200000);
    }

    k93_log("launchd", "stopping all jobs");
    for (int i = 0; i < njobs; i++) stop_job(&J[i]);
    return 0;
}
