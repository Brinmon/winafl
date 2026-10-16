/* harness-template/harness.c -- minimal fork-server harness.
 *
 * Copy this file, replace setup()/fuzz_one(), build with build.cmd, then:
 *   afl-fuzz.exe -fork -i in -o out -t 5000 -- harness.exe @@
 *
 * `setup` runs ONCE before the snapshot point: load the closed-source DLL and
 * resolve the parse function here. `fuzz_one` runs inside a fresh clone for every
 * test case, so it may freely dirty any private state.
 */
#include "../include/fork_harness.h"

#include <stdio.h>
#include <stdlib.h>
#include <windows.h>

/* ---- target-specific part ------------------------------------------------- */

typedef int (*PFN_parse)(const void *buf, size_t len);

static HMODULE        g_dll;
static PFN_parse      g_parse;

static void setup(void)
{
    /* Everything the target needs must be loaded HERE: after this point we are at
     * the snapshot, and the clone must not have to load anything. */
    g_dll = LoadLibraryA("target.dll");
    if (!g_dll) {
        fprintf(stderr, "[harness] LoadLibrary(target.dll) failed: %lu\n", GetLastError());
        ExitProcess(10);
    }
    g_parse = (PFN_parse)GetProcAddress(g_dll, "target_parse");
    if (!g_parse) {
        fprintf(stderr, "[harness] target_parse not found\n");
        ExitProcess(11);
    }
    /* Warm-up: parse a tiny buffer so lazy initialisation happens before the
     * snapshot (avoids holding internal locks in the clone). */
    unsigned char warm[4] = {0};
    g_parse(warm, sizeof(warm));
}

static void fuzz_one(const char *input_file)
{
    FILE *f = fopen(input_file, "rb");
    if (!f) return;
    static unsigned char buf[65536];
    size_t n = fread(buf, 1, sizeof(buf), f);
    fclose(f);
    if (n == 0) return;
    g_parse(buf, n);
}

/* -------------------------------------------------------------------------- */

FORK_HARNESS(setup, fuzz_one)
