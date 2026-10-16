/* harness_crash_fork.c -- P2.5 crash-classification harness.
 *
 * A tiny "target" that deliberately faults so the fork loop can be validated
 * against all three crash classes required by DEVELOPMENT-PLAN.md P2.5:
 *
 *   WINAFL_FORK_CRASH_MODE=0  clean (control)
 *   WINAFL_FORK_CRASH_MODE=1  int3                      -> 0x80000003
 *   WINAFL_FORK_CRASH_MODE=2  access violation (null)    -> 0xC0000005
 *   WINAFL_FORK_CRASH_MODE=3  stack overflow (recursion) -> 0xC00000FD
 *   WINAFL_FORK_CRASH_MODE=4  exit(1) from the clone      (non-exception deviation)
 *
 * The mode is read once in setup(); fuzz_one() acts on it, so the fault happens
 * inside the clone -- exactly the case the parent must survive and classify.
 *
 * ASCII only.
 */
#include "../include/fork_harness.h"

#include <stdio.h>
#include <stdlib.h>
#include <windows.h>

static int g_mode;

static void setup(void)
{
    const char *m = getenv("WINAFL_FORK_CRASH_MODE");
    g_mode = m ? atoi(m) : 0;
}

/* Deterministic stack overflow: optimization is disabled so the compiler cannot
 * fold this into a loop or a tail call, and the frame is touched every page so
 * the guard page is really hit. */
#pragma optimize("", off)
static void recurse_forever(int depth)
{
    volatile char pad[16384];
    int i;
    for (i = 0; i < (int)sizeof(pad); i += 4096) pad[i] = (char)depth;
    recurse_forever(depth + 1);
    pad[0] = (char)depth;   /* keeps this frame alive across the call */
}
#pragma optimize("", on)

static void fuzz_one(const char *input_file)
{
    /* Touch the input so the clone does real file I/O like a production target. */
    FILE *f = fopen(input_file, "rb");
    if (f) {
        char buf[256];
        (void)fread(buf, 1, sizeof(buf), f);
        fclose(f);
    }

    switch (g_mode) {
    case 1:
        __debugbreak();                       /* int3 */
        break;
    case 2: {
        volatile int *p = NULL;
        *p = 1;                               /* access violation */
        break;
    }
    case 3:
        recurse_forever(1);                   /* stack overflow */
        break;
    case 4:
        ExitProcess(1);                       /* plain non-zero exit */
        break;
    default:
        break;                                /* clean */
    }
}

FORK_HARNESS(setup, fuzz_one)
