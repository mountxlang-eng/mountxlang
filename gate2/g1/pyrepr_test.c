// Copyright (C) 2026-2027 Ziad Mohsin. All Rights Reserved.
/* G4: mx_py_repr against the 9 measured values of COMPILE_DESIGN §6.6.
 * These cannot live in a .mx case - 5e-324 is a subnormal and str(2**70) is
 * not representable - so they are a unit test. Compiled and run separately. */

/* The POSIX feature macro, BEFORE every include and for the same reason mini.c
 * needs it: glibc's features.h is read by the FIRST include and caches what it
 * found, so a definition after that has no effect. This file includes mini.c on
 * line 13, long after the standard headers above would have frozen the macros -
 * so without its own, the time natives in mini.c (clock_gettime, nanosleep) are
 * invisible here while compiling perfectly in mini.c's own build. */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>

/* the two implementations under comparison */
#define main mini_main_unused
#include "mini.c"
#undef main

typedef struct { double v; const char *want; } Case;

static const Case CASES[] = {
    { 0.1 + 0.2,   "0.30000000000000004" },
    { 1.5,         "1.5" },
    { 2.0,         "2.0" },
    { 1e-05,       "1e-05" },
    { 1.0 / 3.0,   "0.3333333333333333" },
    { 1e20,        "1e+20" },
    { 1e16,        "1e+16" },
    { 5e-324,      "5e-324" },
    { 0.0,         "0.0" }
};

int main(void) {
    int i, bad = 0;
    for (i = 0; i < (int)(sizeof CASES / sizeof CASES[0]); i++) {
        char got[64];
        mx_py_repr(CASES[i].v, got, sizeof got);
        if (strcmp(got, CASES[i].want) == 0) {
            printf("OK   %-22s -> %s\n", CASES[i].want, got);
        } else {
            printf("FAIL want %-22s got %s\n", CASES[i].want, got);
            bad++;
        }
    }
    printf("\n");
    if (bad == 0) { printf("PYREPR OK - %d value(s) match\n", (int)(sizeof CASES / sizeof CASES[0])); return 0; }
    printf("PYREPR FAILED - %d mismatch(es)\n", bad);
    return 1;
}