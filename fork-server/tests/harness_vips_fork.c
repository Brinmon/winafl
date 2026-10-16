/* harness_vips_fork.c -- fork-server harness for QQ's libvips-42.dll. */
#include "../include/fork_harness.h"
#include <stdio.h>
#include <stdlib.h>
#include <windows.h>

typedef void *(*PFN_new_from_buffer)(const void *, size_t, const char *, ...);
typedef void  (*PFN_init)(const char *);
static PFN_new_from_buffer g_parse;

static void setup(void)
{
    fprintf(stderr, "[harness] loading libvips-42.dll\n");
    fflush(stderr);
    HMODULE h = LoadLibraryA("libvips-42.dll");
    if (!h) { fprintf(stderr, "[harness] libvips load failed: %lu\n", GetLastError()); fflush(stderr); ExitProcess(10); }
    fprintf(stderr, "[harness] libvips loaded @%p\n", (void *)h);
    fflush(stderr);
    g_parse = (PFN_new_from_buffer)GetProcAddress(h, "vips_image_new_from_buffer");
    if (!g_parse) { fprintf(stderr, "[harness] no vips_image_new_from_buffer\n"); fflush(stderr); ExitProcess(11); }
    PFN_init vi = (PFN_init)GetProcAddress(h, "vips_init");
    if (vi) vi("vips_fork");
    fprintf(stderr, "[harness] vips_init done, warmup...\n");
    fflush(stderr);
    /* warmup before the snapshot point */
    unsigned char warm[8] = {0};
    g_parse(warm, sizeof(warm), "", NULL);
    fprintf(stderr, "[harness] warmup done\n");
    fflush(stderr);
}

static void fuzz_one(const char *input_file)
{
    FILE *f = fopen(input_file, "rb");
    if (!f) return;
    static unsigned char buf[65536];
    size_t n = fread(buf, 1, sizeof(buf), f);
    fclose(f);
    if (n == 0) return;
    g_parse(buf, n, "", NULL);
}

FORK_HARNESS(setup, fuzz_one)
