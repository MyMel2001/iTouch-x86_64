/* Calculator.app - clean-room counterpart of iOS 4.0 Calculator.
 * Four-function keypad + expression evaluation. Scientific layout, square
 * root and percent are the ceiling of this build. */
#define _GNU_SOURCE
#include "k93ui.h"
#include "k93util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void fmt_num(char *buf, size_t n, double v)
{
    long long iv = (long long)v;
    if ((double)iv == v && v >= -9000000000000000.0 && v <= 9000000000000000.0)
        snprintf(buf, n, "%lld", iv);
    else
        snprintf(buf, n, "%.10g", v);
}

static const char *keys[] = {
    "C", "(", ")", "/",
    "7", "8", "9", "*",
    "4", "5", "6", "-",
    "1", "2", "3", "+",
    "0", ".", "<-", "=",
};
#define NK (int)(sizeof keys / sizeof keys[0])

int main(void)
{
    if (ui_init("Calculator") < 0) return 1;
    char expr[160] = "";
    for (;;) {
        char title[160];
        snprintf(title, sizeof title, "%s", expr[0] ? expr : "0");
        int sel = ui_menu(title, keys, NK, 0);
        if (sel < 0) break;
        const char *k = keys[sel];
        if (!strcmp(k, "C")) { expr[0] = 0; }
        else if (!strcmp(k, "<-")) { size_t l = strlen(expr); if (l) expr[l - 1] = 0; }
        else if (!strcmp(k, "=")) {
            double v;
            if (k93_calc_eval(expr, &v) == 0) fmt_num(expr, sizeof expr, v);
            else snprintf(expr, sizeof expr, "Error");
        }
        else {
            size_t l = strlen(expr);
            if (l + 2 < sizeof expr) { expr[l] = k[0]; expr[l + 1] = 0; }
        }
    }
    ui_shutdown();
    return 0;
}
