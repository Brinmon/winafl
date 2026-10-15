// cov_stability.c -- measure REAL coverage stability using a hardware-free
// sampling profiler: a watchdog thread samples the instruction pointer of the
// fuzzing thread, building a coarse "coverage" set. Run the same input N times
// and compare sets -> this reproduces AFL's stability metric in spirit.
//
// persist mode : same process, N parses
// fork mode    : clone per parse (clean state every time)
#include <windows.h>
#include <psapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef LONG NTSTATUS;
#define STATUS_SUCCESS        ((NTSTATUS)0x00000000L)
#define STATUS_PROCESS_CLONED  ((NTSTATUS)0x00000129L)
#define RTL_CLONE_PROCESS_FLAGS_INHERIT_HANDLES 0x00000002
typedef struct _RTL_USER_PROCESS_INFORMATION { ULONG Length; HANDLE P; HANDLE T; BYTE R[512]; } RTL_USER_PROCESS_INFORMATION;
typedef NTSTATUS (NTAPI *PFN_clone)(ULONG, PSECURITY_DESCRIPTOR, PSECURITY_DESCRIPTOR, HANDLE, RTL_USER_PROCESS_INFORMATION *);
typedef void *(*PFN_parse)(const void *, size_t, const char *, ...);
typedef void  (*PFN_init)(const char *);

static PFN_clone p_clone; static PFN_parse p_parse;
static unsigned char *g_input; static size_t g_input_len;
static volatile LONG g_stop; static HANDLE g_target_thread;
static unsigned char *g_pcset;            // 1 byte per 4KB bucket of libvips image
static unsigned long long g_mod_base, g_mod_size;

// 16K buckets of coverage
#define NBUCK 16384
static volatile LONG g_cov[NBUCK];
static volatile LONG *g_shared_cov;        // shared across the clone

static DWORD WINAPI sampler(LPVOID unused)
{
    (void)unused;
    CONTEXT ctx;
    while (!g_stop) {
        if (g_target_thread) {
            memset(&ctx, 0, sizeof(ctx));
            ctx.ContextFlags = CONTEXT_CONTROL;
            if (SuspendThread(g_target_thread) != (DWORD)-1) {
                if (GetThreadContext(g_target_thread, &ctx)) {
                    unsigned long long ip = ctx.Rip;
                    if (ip >= g_mod_base && ip < g_mod_base + g_mod_size) {
                        unsigned idx = (unsigned)((ip - g_mod_base) / 64) % NBUCK;
                        if (g_shared_cov) InterlockedIncrement((LONG *)&g_shared_cov[idx]);
                        else InterlockedIncrement((LONG *)&g_cov[idx]);
                    }
                }
                ResumeThread(g_target_thread);
            }
        }
        Sleep(0);
    }
    return 0;
}

static unsigned long long fingerprint(const volatile LONG *cov)
{
    unsigned long long h = 1469598103934665603ULL;
    for (int i = 0; i < NBUCK; i++) {
        unsigned v = (unsigned)cov[i];
        if (v) { h ^= (unsigned)i * 2654435761u; h *= 1099511628211ULL; }
    }
    return h;
}
static int nonzero(const volatile LONG *cov) {
    int n = 0; for (int i = 0; i < NBUCK; i++) if (cov[i]) n++; return n;
}
static void clear_cov(volatile LONG *cov) { for (int i = 0; i < NBUCK; i++) cov[i] = 0; }

int main(int argc, char **argv)
{
    const char *mode = "persist", *seed = NULL; int n = 100; int dir_set = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--fork")) mode = "fork";
        else if (!strcmp(argv[i], "--persist")) mode = "persist";
        else if (!dir_set) dir_set = 1;
        else if (!seed) seed = argv[i];
        else n = atoi(argv[i]);
    }
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    p_clone = (PFN_clone)GetProcAddress(GetModuleHandleA("ntdll.dll"), "RtlCloneUserProcess");
    HMODULE hv = LoadLibraryA("libvips-42.dll");
    if (!hv) { printf("FATAL libvips\n"); return 3; }
    p_parse = (PFN_parse)GetProcAddress(hv, "vips_image_new_from_buffer");
    PFN_init vi = (PFN_init)GetProcAddress(hv, "vips_init"); if (vi) vi("cov");
    MODULEINFO mi; memset(&mi, 0, sizeof(mi));
    GetModuleInformation(GetCurrentProcess(), hv, &mi, sizeof(mi));
    g_mod_base = (unsigned long long)mi.lpBaseOfDll; g_mod_size = mi.SizeOfImage;

    FILE *f = fopen(seed, "rb"); if (!f) { printf("FATAL seed\n"); return 4; }
    static unsigned char buf[65536]; g_input_len = fread(buf, 1, sizeof(buf), f); fclose(f); g_input = buf;

    HANDLE hm = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, NBUCK*4, "Local\covstab");
    g_shared_cov = (volatile LONG *)MapViewOfFile(hm, FILE_MAP_ALL_ACCESS, 0, 0, NBUCK*4);

    int ok0; { p_parse(g_input, g_input_len, "", NULL); (void)ok0; }   // warmup

    // sampler thread
    g_target_thread = GetCurrentThread();
    HANDLE th = CreateThread(NULL, 0, sampler, NULL, 0, NULL);
    Sleep(20);

    printf("mode=%s seed=%s (%zu bytes) iters=%d\n", mode, seed, g_input_len, n);
    printf("libvips base=%llx size=%llx  buckets=%d\n\n", g_mod_base, g_mod_size, NBUCK);

    // collect N fingerprints of the SAME input
    unsigned long long fps[256]; int nfp = 0; int buckets[256];
    DWORD t0 = GetTickCount();
    for (int i = 0; i < n && nfp < 256; i++) {
        volatile LONG *cov = (mode[0]=='f') ? g_shared_cov : g_cov;
        clear_cov(cov);
        if (mode[0] == 'f') {
            RTL_USER_PROCESS_INFORMATION info; memset(&info,0,sizeof(info)); info.Length=sizeof(info);
            NTSTATUS st = p_clone(RTL_CLONE_PROCESS_FLAGS_INHERIT_HANDLES, NULL, NULL, NULL, &info);
            if (st == STATUS_PROCESS_CLONED) {
                // in the clone: sample THIS thread's own execution
                g_target_thread = GetCurrentThread();
                g_shared_cov = (volatile LONG *)MapViewOfFile(hm, FILE_MAP_ALL_ACCESS, 0, 0, NBUCK*4);
                clear_cov(g_shared_cov);
                HANDLE ct = CreateThread(NULL, 0, sampler, NULL, 0, NULL);
                Sleep(5);
                p_parse(g_input, g_input_len, "", NULL);
                Sleep(5);
                g_stop = 1; WaitForSingleObject(ct, 1000); CloseHandle(ct);
                ExitProcess(0);
            }
            if (st == STATUS_SUCCESS) {
                WaitForSingleObject(info.P, 10000);
                CloseHandle(info.T); CloseHandle(info.P);
            }
        } else {
            p_parse(g_input, g_input_len, "", NULL);
        }
        fps[nfp] = fingerprint(cov); buckets[nfp] = nonzero(cov); nfp++;
    }
    g_stop = 1; WaitForSingleObject(th, 1000); CloseHandle(th);
    DWORD dt = GetTickCount() - t0;

    // stability: how many fingerprints equal the most common one
    int best = 0;
    for (int i = 0; i < nfp; i++) {
        int c = 0; for (int j = 0; j < nfp; j++) if (fps[j] == fps[i]) c++;
        if (c > best) best = c;
    }
    int distinct = 0;
    for (int i = 0; i < nfp; i++) {
        int seen = 0; for (int j = 0; j < i; j++) if (fps[j]==fps[i]) { seen=1; break; }
        if (!seen) distinct++;
    }
    int bmin = 1<<30, bmax = 0, bsum = 0;
    for (int i = 0; i < nfp; i++) { if (buckets[i]<bmin) bmin=buckets[i]; if (buckets[i]>bmax) bmax=buckets[i]; bsum+=buckets[i]; }

    printf("  runs            : %d\n", nfp);
    printf("  distinct sets   : %d\n", distinct);
    printf("  STABILITY       : %.1f%%   <- %d/%d runs identical\n", 100.0*best/nfp, best, nfp);
    printf("  coverage buckets: min=%d avg=%.0f max=%d\n", bmin, (double)bsum/nfp, bmax);
    printf("  time            : %lums (%.0f runs/sec)\n", dt, nfp*1000.0/(dt?dt:1));
    return 0;
}
