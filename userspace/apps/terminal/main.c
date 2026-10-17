/* Terminal.app - hand the console to a root shell.
 * iOS 4.3.4 had no terminal; this OS ships one because root access is a
 * first-class feature. It suspends the UI and runs busybox sh on the current
 * controlling terminal. */
#define _GNU_SOURCE
#include "k93ui.h"
#include "k93util.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>

int main(void)
{
    printf("\nK93OS Terminal - type 'exit' to return to SpringBoard.\n\n");
    fflush(stdout);
    pid_t p = fork();
    if (p == 0) {
        execl("/bin/busybox", "busybox", "sh", "-l", (char *)NULL);
        execl("/bin/sh", "sh", "-l", (char *)NULL);
        _exit(127);
    }
    int st = 0;
    waitpid(p, &st, 0);
    return 0;
}
