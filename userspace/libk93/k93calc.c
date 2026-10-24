/* k93calc.c - tiny four-function expression evaluator (clean-room).
 * Recursive descent: + - * / ( ) and unary +/-. Used by Calculator.app.
 * No memory allocation, no globals leaked to the caller. */
#include "k93util.h"
#include <stdlib.h>

static const char *P;
static int perr;

static void skipsp(void) { while (*P == ' ') P++; }
static double add(void);

static double prim(void)
{
    skipsp();
    if (*P == '(') {
        P++;
        double v = add();
        skipsp();
        if (*P == ')') P++; else perr = 1;
        return v;
    }
    if (*P == '+' || *P == '-') {
        int neg = (*P == '-');
        P++;
        double v = prim();
        return neg ? -v : v;
    }
    char *end;
    double v = strtod(P, &end);
    if (end == P) perr = 1;
    P = end;
    return v;
}

static double mul(void)
{
    double v = prim();
    for (;;) {
        skipsp();
        if (*P != '*' && *P != '/') break;
        char op = *P++;
        double r = prim();
        if (op == '/') {
            if (r == 0) { perr = 1; return 0; }
            v /= r;
        } else {
            v *= r;
        }
    }
    return v;
}

static double add(void)
{
    double v = mul();
    for (;;) {
        skipsp();
        if (*P != '+' && *P != '-') break;
        char op = *P++;
        double r = mul();
        v = (op == '+') ? v + r : v - r;
    }
    return v;
}

/* Evaluate a NUL-terminated expression. Returns 0 and stores the value in
 * *out on success, or -1 for a syntax error, trailing junk, or divide-by-zero. */
int k93_calc_eval(const char *expr, double *out)
{
    P = expr;
    perr = 0;
    double v = add();
    skipsp();
    if (perr || *P) return -1;
    *out = v;
    return 0;
}
