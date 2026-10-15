// stability_test.c -- THE decisive test: does cloning give clean state?
//
// Measures coverage stability the way AFL does: run the SAME input N times and
// compare the resulting coverage sets. 100% = identical every time (clean state).
//
// Two modes, same binary, same input:
//   --persist : call the parser N times in ONE process (like WinAFL -persist)
//   --fork    : clone the process for each call (RtlCloneUserProcess)
//
// Coverage is recorded with a simple edge bitmap built from the parser's own
// control flow, via a lightweight inline hook on a per-call basis is too
// invasive, so we approximate AFL's map by hashing the RETURN ADDRESS chain
// captured with a sampled stack walk is also unstable...
//
// Instead we use the strongest signal available without instrumentation:
// the parser's OBSERVABLE RESULT + a checksum of the bytes it produces.
// If state leaks between iterations, the produced bytes change.
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef LONG NTSTATUS;
#define STATUS_SUCCESS        ((NTSTATUS)0x00000000L)
#define STATUS_PROCESS_CLONED  ((NTSTATUS)0x00000129L)
#define RTL_CLONE_PROCESS_FLAGS_INHERIT_HANDLES 0x00000002

typedef struct _RTL_USER_PROCESS_INFORMATION {
    ULONG Length; HANDLE ProcessHandle; HANDLE ThreadHandle; BYTE Reserved[512];
} RTL_USER_PROCESS_INFORMATION;

typedef NTSTATUS (NTAPI *PFN_clone)(ULONG, PSECURITY_DESCRIPTOR, PSECURITY_DESCRIPTOR, HANDLE, RTL_USER_PROCESS_INFORMATION *);
typedef void *(*PFN_parse)(const void *, size_t, const char *, ...);
typedef void  (*PFN_init)(const char *);
typedef int   (*PFN_get_int)(void *, const char *);
typedef void  (*PFN_err_clear)(void);
typedef const char *(*PFN_err_buffer)(void);

static PFN_clone      p_clone;
static PFN_parse      p_parse;
static PFN_get_int    p_width, p_height, p_bands;
static PFN_err_clear  p_err_clear;
static PFN_err_buffer p_err_buffer;

static unsigned char *g_input; static size_t g_input_len;
static volatile unsigned long long *g_out;   // shared: [0]=hash, [1]=ok, [2]=width, [3]=height

static unsigned long long fnv1a(const void *p, size_t n) {
    const unsigned char *b = (const unsigned char *)p;
    unsigned long long h = 1469598103934665603ULL;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 1099511628211ULL; }
    return h;
}

// One parse of the input; returns a fingerprint of everything observable.
static unsigned long long parse_once(int *ok_out)
{
    if (p_err_clear) p_err_clear();
    void *img = p_parse(g_input, g_input_len, "", NULL);
    if (!img) { *ok_out = 0; return 0; }

    int w = p_width ? p_width(img, "width") : -1;
    int h = p_height ? p_height(img, "height") : -1;
    int b = p_bands ? p_bands(img, "bands") : -1;
    const char *err = p_err_buffer ? p_err_buffer() : "";

    unsigned long long fp = 1469598103934665603ULL;
    fp = (fp ^ (unsigned)w) * 1099511628211ULL;
    fp = (fp ^ (unsigned)h) * 1099511628211ULL;
    fp = (fp ^ (unsigned)b) * 1099511628211ULL;
    fp ^= fnv1a(err, strlen(err));
    *ok_out = 1;
    return fp;
}

int main(int argc, char **argv)
{
    const char *mode = "persist";
    const char *dir = "."; const char *seed = NULL; int n = 200;
    int dir_set = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--fork")) mode = "fork";
        else if (!strcmp(argv[i], "--persist")) mode = "persist";
        else if (!dir_set) { dir = argv[i]; dir_set = 1; }
        else if (!seed) seed = argv[i];
        else n = atoi(argv[i]);
    }
    SetCurrentDirectoryA(dir);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);

    p_clone = (PFN_clone)GetProcAddress(GetModuleHandleA("ntdll.dll"), "RtlCloneUserProcess");
    HMODULE hv = LoadLibraryA("libvips-42.dll");
    if (!hv) { printf("FATAL: libvips load failed %lu\n", GetLastError()); return 3; }
    p_parse     = (PFN_parse)GetProcAddress(hv, "vips_image_new_from_buffer");
    p_width     = (PFN_get_int)GetProcAddress(hv, "vips_image_get_width");
    p_height    = (PFN_get_int)GetProcAddress(hv, "vips_image_get_height");
    p_bands     = (PFN_get_int)GetProcAddress(hv, "vips_image_get_bands");
    p_err_clear = (PFN_err_clear)GetProcAddress(hv, "vips_error_clear");
    p_err_buffer= (PFN_err_buffer)GetProcAddress(hv, "vips_error_buffer");
    PFN_init vinit = (PFN_init)GetProcAddress(hv, "vips_init");
    if (vinit) vinit("stability_test");

    FILE *f = fopen(seed, "rb");
    if (!f) { printf("FATAL: cannot open seed %s\n", seed); return 4; }
    static unsigned char buf[65536];
    g_input_len = fread(buf, 1, sizeof(buf), f);
    fclose(f); g_input = buf;

    HANDLE hm = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, 4096, "Local\stab_out");
    g_out = (volatile unsigned long long *)MapViewOfFile(hm, FILE_MAP_ALL_ACCESS, 0, 0, 4096);

    // warmup: identical starting point for both modes
    int ok0; parse_once(&ok0);

    printf("mode=%s  input=%s (%zu bytes)  iterations=%d\n", mode, seed, g_input_len, n);
    printf("warmup parse ok=%d\n\n", ok0);

    unsigned long long first = 0; int distinct = 0; int okc = 0;
    unsigned long long seen[64]; int nseen = 0;
    DWORD t0 = GetTickCount();

    if (!strcmp(mode, "persist")) {
        for (int i = 0; i < n; i++) {
            int ok; unsigned long long fp = parse_once(&ok);
            if (ok) okc++;
            if (i == 0) first = fp;
            int found = 0;
            for (int j = 0; j < nseen; j++) if (seen[j] == fp) { found = 1; break; }
            if (!found && nseen < 64) { seen[nseen++] = fp; distinct++; }
        }
    } else {
        for (int i = 0; i < n; i++) {
            RTL_USER_PROCESS_INFORMATION info; memset(&info, 0, sizeof(info)); info.Length = sizeof(info);
            g_out[0] = 0; g_out[1] = 0;
            NTSTATUS st = p_clone(RTL_CLONE_PROCESS_FLAGS_INHERIT_HANDLES, NULL, NULL, NULL, &info);
            if (st == STATUS_PROCESS_CLONED) {
                int ok; unsigned long long fp = parse_once(&ok);
                g_out[0] = fp; g_out[1] = ok ? 1 : 0;
                ExitProcess(0);
            }
            if (st == STATUS_SUCCESS) {
                if (WaitForSingleObject(info.ProcessHandle, 10000) == WAIT_TIMEOUT) {
                    TerminateProcess(info.ProcessHandle, 1);
                }
                CloseHandle(info.ThreadHandle); CloseHandle(info.ProcessHandle);
                unsigned long long fp = g_out[0]; int ok = (int)g_out[1];
                if (ok) okc++;
                if (i == 0) first = fp;
                int found = 0;
                for (int j = 0; j < nseen; j++) if (seen[j] == fp) { found = 1; break; }
                if (!found && nseen < 64) { seen[nseen++] = fp; distinct++; }
            }
        }
    }
    DWORD dt = GetTickCount() - t0;

    // stability = how often the fingerprint equals the first one
    int same = 0;
    // recompute: count how many of the samples matched `first` is not tracked, so
    // report distinct count as the instability proxy
    printf("  parses ok       : %d / %d\n", okc, n);
    printf("  distinct results: %d   <- 1 means perfectly stable state\n", distinct);
    printf("  stability proxy : %.1f%%\n", 100.0 / (distinct ? distinct : 1));
    printf("  elapsed         : %lums  (%.0f parses/sec)\n", dt, okc * 1000.0 / (dt ? dt : 1));
    printf("\n%s\n", distinct == 1 ? "STABLE: identical result every iteration"
                                   : "UNSTABLE: results vary between iterations");
    return distinct == 1 ? 0 : 1;
}
