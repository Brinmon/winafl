/*
 * probe_r4.c -- attribution experiment R4
 * Layer isolation: does behavior drift across repeated in-process calls?
 * (persistent-mode state accumulation, independent of any instrumentation)
 *
 * Usage: probe_r4.exe -n <iters> [-u] [-c] [-s] [-e] <input>
 *   -u  g_object_unref(image) after each call      (harness fix #1)
 *   -c  vips_cache_set_max(0)                      (harness fix #2)
 *   -s  vips_concurrency_set(1)                    (harness fix #3)
 *   -e  force evaluation: vips_image_write_to_memory + FNV checksum
 *   -r  vips_error_clear() after each call  (NOT what the real harness does)
 *
 * Per-iteration CSV line:
 *   iter,ms,width,cache_size,threads,privKB,errlen,im_ptr[,fnv]
 * P0 (no flags) reproduces the CURRENT harness behavior exactly.
 */
#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#pragma comment(lib, "psapi.lib")

typedef void *(*PFN_newbuf)(const void *, size_t, const char *, ...);
typedef void (*PFN_vips_init)(const char *);
typedef int (*PFN_cache_size)(void);
typedef int (*PFN_cache_max)(void);
typedef void (*PFN_cache_set_max)(int);
typedef const char *(*PFN_errbuf)(void);
typedef void (*PFN_errclr)(void);
typedef int (*PFN_conc_get)(void);
typedef void (*PFN_conc_set)(int);
typedef int (*PFN_getwidth)(void *);
typedef void (*PFN_unref)(void *);
typedef void *(*PFN_writemem)(void *, size_t *);

static int count_threads(void)
{
    DWORD pid = GetCurrentProcessId();
    int n = 0;
    HANDLE h = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te;
    te.dwSize = sizeof(te);
    if (h != INVALID_HANDLE_VALUE) {
        if (Thread32First(h, &te)) {
            do {
                if (te.th32OwnerProcessID == pid) n++;
            } while (Thread32Next(h, &te));
        }
        CloseHandle(h);
    }
    return n;
}

static size_t priv_kb(void)
{
    PROCESS_MEMORY_COUNTERS_EX pm;
    pm.cb = sizeof(pm);
    if (GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *)&pm, sizeof(pm)))
        return pm.PrivateUsage / 1024;
    return 0;
}

static unsigned long long fnv1a(const unsigned char *p, size_t n)
{
    unsigned long long h = 1469598103934665603ULL;
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 1099511628211ULL; }
    return h;
}

static long long qpc_ms(void)
{
    static LARGE_INTEGER f = {0}, t;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    return (long long)(t.QuadPart * 1000 / f.QuadPart);
}

int main(int argc, char **argv)
{
    int iters = 100, do_unref = 0, do_cacheoff = 0, do_single = 0, do_eval = 0, do_errclr = 0;
    const char *input = NULL;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-n") && i + 1 < argc) iters = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-u")) do_unref = 1;
        else if (!strcmp(argv[i], "-c")) do_cacheoff = 1;
        else if (!strcmp(argv[i], "-s")) do_single = 1;
        else if (!strcmp(argv[i], "-e")) do_eval = 1;
        else if (!strcmp(argv[i], "-r")) do_errclr = 1;
        else input = argv[i];
    }
    if (!input) { fprintf(stderr, "usage: probe_r4 -n N [-u] [-c] [-s] [-e] <input>\n"); return 1; }

    HMODULE h = LoadLibraryA("libvips-42.dll");
    if (!h) { fprintf(stderr, "LoadLibrary libvips-42.dll failed: %lu\n", GetLastError()); return 2; }
    HMODULE hg = LoadLibraryA("libgobject-2.0-0.dll");

#define GPA(mod, name) (name ? (void *)GetProcAddress(mod, name) : NULL)
    PFN_newbuf     p_newbuf     = (PFN_newbuf)GetProcAddress(h, "vips_image_new_from_buffer");
    PFN_vips_init  p_init       = (PFN_vips_init)GetProcAddress(h, "vips_init");
    PFN_cache_size p_csize      = (PFN_cache_size)GetProcAddress(h, "vips_cache_get_size");
    PFN_cache_max  p_cmax       = (PFN_cache_max)GetProcAddress(h, "vips_cache_get_max");
    PFN_cache_set_max p_csetmax = (PFN_cache_set_max)GetProcAddress(h, "vips_cache_set_max");
    PFN_errbuf     p_errbuf     = (PFN_errbuf)GetProcAddress(h, "vips_error_buffer");
    PFN_errclr     p_errclr     = (PFN_errclr)GetProcAddress(h, "vips_error_clear");
    PFN_conc_get   p_concget    = (PFN_conc_get)GetProcAddress(h, "vips_concurrency_get");
    PFN_conc_set   p_concset    = (PFN_conc_set)GetProcAddress(h, "vips_concurrency_set");
    PFN_getwidth   p_width      = (PFN_getwidth)GetProcAddress(h, "vips_image_get_width");
    PFN_writemem   p_writemem   = (PFN_writemem)GetProcAddress(h, "vips_image_write_to_memory");
    PFN_unref      p_unref      = hg ? (PFN_unref)GetProcAddress(hg, "g_object_unref") : NULL;
#undef GPA

    if (!p_newbuf || !p_init) { fprintf(stderr, "missing exports\n"); return 3; }

    p_init("probe_r4");
    if (do_cacheoff && p_csetmax) p_csetmax(0);
    if (do_single && p_concset)   p_concset(1);

    /* read whole input once (same static-buffer semantics as harness_vips.c) */
    FILE *f = fopen(input, "rb");
    if (!f) { fprintf(stderr, "open %s failed\n", input); return 4; }
    static unsigned char buf[65536];
    size_t n = fread(buf, 1, sizeof(buf), f);
    fclose(f);

    printf("# cfg: iters=%d unref=%d cacheoff=%d single=%d eval=%d input=%s bytes=%zu\n",
           iters, do_unref, do_cacheoff, do_single, do_eval, input, n);
    printf("# conc_init=%d cache_max=%d threads0=%d priv0=%zuKB\n",
           p_concget ? p_concget() : -1, p_cmax ? p_cmax() : -1, count_threads(), priv_kb());
    printf("iter,ms,width,cache,threads,privKB,errlen,im_ptr,fnv\n");

    unsigned long long prev_fnv = 0;
    int fnv_diffs = 0;

    for (int i = 0; i < iters; i++) {
        long long t0 = qpc_ms();
        void *im = p_newbuf(buf, n, "", NULL);
        long long ms = qpc_ms() - t0;

        int width = (im && p_width) ? p_width(im) : -1;
        unsigned long long fv = 0;
        if (do_eval && im && p_writemem) {
            size_t len = 0;
            void *mem = p_writemem(im, &len);
            if (mem && len) fv = fnv1a((const unsigned char *)mem, len);
            if (i > 0 && prev_fnv && fv && fv != prev_fnv) fnv_diffs++;
            if (fv) prev_fnv = fv;
        }

        printf("%d,%d,%d,%d,%d,%zu,%zu,%p,%llu\n",
               i, (int)ms, width,
               p_csize ? p_csize() : -1,
               count_threads(), priv_kb(),
               strlen(p_errbuf ? p_errbuf() : ""), im, fv);

        if (do_unref && im && p_unref) p_unref(im);
        if (do_errclr && p_errclr) p_errclr();  /* real harness does NOT clear */
    }

    if (do_eval) printf("# fnv_diffs(out of %d)= %d\n", iters - 1, fnv_diffs);
    return 0;
}
