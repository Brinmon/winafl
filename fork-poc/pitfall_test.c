// pitfall_test.c -- empirically test the 5 documented process-cloning pitfalls
// using the REAL closed-source target (QQ's libvips-42.dll), not a toy.
//
// Pitfalls under test (source: huntandhackett "Definitive Guide To Process Cloning"):
//   P1 handle-table holes    (exclusive handles / ALPC ports not duplicated)
//   P2 other threads not cloned (and thread pools keep spawning in the parent)
//   P3 synchronization primitives may deadlock in the clone
//   P4 loading DLLs inside the clone hangs (must preload)
//   P5 mapped views may be ViewUnmap instead of ViewShare (our coverage map!)
//
// Plus the two properties we actually need:
//   A) repeated cloning does not leak handles/memory in the parent
//   B) the clone can call the real libvips parser without deadlocking
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef LONG NTSTATUS;
#define STATUS_SUCCESS        ((NTSTATUS)0x00000000L)
#define STATUS_PROCESS_CLONED  ((NTSTATUS)0x00000129L)
#define RTL_CLONE_PROCESS_FLAGS_CREATE_SUSPENDED 0x00000001
#define RTL_CLONE_PROCESS_FLAGS_INHERIT_HANDLES  0x00000002
#define RTL_CLONE_PROCESS_FLAGS_NO_SYNCHRONIZE   0x00000004

typedef struct _RTL_USER_PROCESS_INFORMATION {
    ULONG Length; HANDLE ProcessHandle; HANDLE ThreadHandle; BYTE Reserved[512];
} RTL_USER_PROCESS_INFORMATION;

typedef NTSTATUS (NTAPI *PFN_RtlCloneUserProcess)(ULONG, PSECURITY_DESCRIPTOR,
    PSECURITY_DESCRIPTOR, HANDLE, RTL_USER_PROCESS_INFORMATION *);
typedef void *(*PFN_new_from_buffer)(const void *, size_t, const char *, ...);
typedef void  (*PFN_vips_init)(const char *);

static PFN_RtlCloneUserProcess g_clone;
static PFN_new_from_buffer    g_parse;
static unsigned char         *g_input;      // input bytes (shared, read-only in practice)
static size_t                 g_input_len;
static unsigned char         *g_cov;        // shared coverage map (must survive the clone)
static volatile LONG         *g_ctl;        // shared control block

#define CTL_RUNS      0   // parent: iterations completed
#define CTL_CLONE_OK  1   // clone: parse succeeded
#define CTL_CLONE_ERR 2   // clone: parse returned NULL
#define CTL_DLL_LOAD  3   // clone: LoadLibrary attempt result
#define CTL_HANDLES   4   // clone: handle count observed
#define CTL_WORDS     16

static DWORD count_handles(void) {
    DWORD n = 0;
    GetProcessHandleCount(GetCurrentProcess(), &n);
    return n;
}

static int run_one_clone(const char *label, int try_load_dll_in_clone, DWORD timeout_ms)
{
    RTL_USER_PROCESS_INFORMATION info;
    memset(&info, 0, sizeof(info));
    info.Length = sizeof(info);

    NTSTATUS st = g_clone(RTL_CLONE_PROCESS_FLAGS_INHERIT_HANDLES, NULL, NULL, NULL, &info);

    if (st == STATUS_PROCESS_CLONED) {
        // ---------- CLONE ----------
        DWORD hc = count_handles();
        InterlockedExchange((LONG *)&g_ctl[CTL_HANDLES], (LONG)hc);

        // P4: can we load a DLL in the clone?
        if (try_load_dll_in_clone) {
            HMODULE m = LoadLibraryA("libgobject-2.0-0.dll");
            InterlockedExchange((LONG *)&g_ctl[CTL_DLL_LOAD], m ? 1 : 0);
        }

        // P3 + B: call the real parser. If cloning left a lock held, this hangs.
        if (g_parse && g_input && g_input_len) {
            void *img = g_parse(g_input, g_input_len, "", NULL);
            InterlockedExchange((LONG *)&g_ctl[CTL_CLONE_OK], img ? 1 : 0);
            InterlockedExchange((LONG *)&g_ctl[CTL_CLONE_ERR], img ? 0 : 1);
        }

        // P5: write to the shared coverage map; the parent must see it.
        g_cov[0] = 0x5A;
        g_cov[1] = (unsigned char)(hc & 0xFF);
        ExitProcess(0);
    }

    if (st == STATUS_SUCCESS) {
        // ---------- PARENT ----------
        DWORD wait = WaitForSingleObject(info.ProcessHandle, timeout_ms);
        DWORD code = 0;
        GetExitCodeProcess(info.ProcessHandle, &code);
        CloseHandle(info.ThreadHandle);
        CloseHandle(info.ProcessHandle);
        if (wait == WAIT_TIMEOUT) {
            printf("    [%s] TIMEOUT (deadlock?) after %lums\n", label, timeout_ms);
            return 0;
        }
        InterlockedIncrement((LONG *)&g_ctl[CTL_RUNS]);
        return 1;
    }

    printf("    [%s] clone failed: 0x%08lX\n", label, (unsigned long)st);
    return 0;
}

int main(int argc, char **argv)
{
    const char *dll_dir = (argc > 1) ? argv[1] : ".";
    const char *seed    = (argc > 2) ? argv[2] : NULL;
    int iterations      = (argc > 3) ? atoi(argv[3]) : 200;

    SetCurrentDirectoryA(dll_dir);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);

    g_clone = (PFN_RtlCloneUserProcess)GetProcAddress(GetModuleHandleA("ntdll.dll"), "RtlCloneUserProcess");
    if (!g_clone) { printf("FATAL: no RtlCloneUserProcess\n"); return 2; }

    // ---- preload EVERYTHING before the snapshot point (pitfall P4) ----
    HMODULE hv = LoadLibraryA("libvips-42.dll");
    if (!hv) { printf("FATAL: LoadLibrary libvips failed: %lu (cwd=%s)\n", GetLastError(), dll_dir); return 3; }
    g_parse = (PFN_new_from_buffer)GetProcAddress(hv, "vips_image_new_from_buffer");
    PFN_vips_init vinit = (PFN_vips_init)GetProcAddress(hv, "vips_init");
    if (vinit) vinit("pitfall_test");

    // ---- shared coverage map (pitfall P5) ----
    HANDLE hm = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, 65536, "Local\pitfall_cov");
    g_cov = (unsigned char *)MapViewOfFile(hm, FILE_MAP_ALL_ACCESS, 0, 0, 65536);
    HANDLE hc = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, 4096, "Local\pitfall_ctl");
    g_ctl = (volatile LONG *)MapViewOfFile(hc, FILE_MAP_ALL_ACCESS, 0, 0, 4096);
    if (!g_cov || !g_ctl) { printf("FATAL: shared map failed\n"); return 4; }
    memset((void *)g_ctl, 0, 64);

    // ---- load the seed image ----
    if (seed) {
        FILE *f = fopen(seed, "rb");
        if (f) {
            static unsigned char buf[65536];
            g_input_len = fread(buf, 1, sizeof(buf), f);
            fclose(f);
            g_input = buf;
            printf("seed: %s (%zu bytes)\n", seed, g_input_len);
        }
    }

    // ---- warmup: get libvips into a steady state BEFORE the snapshot ----
    if (g_parse && g_input) { g_parse(g_input, g_input_len, "", NULL); }

    DWORD h0 = count_handles();
    printf("pre-clone: handles=%lu  parse=%p\n", h0, (void *)g_parse);

    // ---- P3/B: repeated cloning, watching for deadlock + leaks ----
    printf("\n== P2/P3/B: %d clones with real libvips parse (timeout 5s each) ==\n", iterations);
    int ok = 0, timed_out = 0;
    DWORD t0 = GetTickCount();
    for (int i = 0; i < iterations; i++) {
        DWORD before = count_handles();
        if (!run_one_clone("run", 0, 5000)) timed_out++;
        else ok++;
        if ((i + 1) % 50 == 0) {
            DWORD now = count_handles();
            printf("    iter %4d: handles=%lu (start %lu)  ok=%d timeout=%d  cov=[%02X %02X]\n",
                   i + 1, now, before, ok, timed_out, g_cov[0], g_cov[1]);
        }
    }
    DWORD dt = GetTickCount() - t0;
    DWORD h1 = count_handles();
    double rate = (dt > 0) ? (ok * 1000.0 / dt) : 0;

    printf("\n== results ==\n");
    printf("  clones ok      : %d / %d\n", ok, iterations);
    printf("  timeouts       : %d   <- P3 deadlock indicator\n", timed_out);
    printf("  parse succeeded: %ld (last clone)  <- B: real parser works in the clone\n", g_ctl[CTL_CLONE_OK]);
    printf("  clone handle n : %ld\n", g_ctl[CTL_HANDLES]);
    printf("  parent handles : %lu -> %lu   <- A: leak indicator (must be equal)\n", h0, h1);
    printf("  cov written by clone, read by parent: [%02X %02X]  <- P5 shared map survives\n", g_cov[0], g_cov[1]);
    printf("  rate           : %.0f clones/sec\n", rate);

    // ---- P4: DLL load inside the clone ----
    printf("\n== P4: LoadLibrary inside the clone ==\n");
    memset((void *)g_ctl, 0, 64);
    if (run_one_clone("load", 1, 5000)) {
        printf("  clone completed; LoadLibrary inside clone %s\n",
               g_ctl[CTL_DLL_LOAD] ? "SUCCEEDED" : "FAILED");
   
    } else {
        printf("  clone TIMED OUT when loading a DLL inside -> P4 confirmed (must preload)\n");
    }

    int pass = (ok == iterations) && (h1 == h0) && (g_cov[0] == 0x5A);
    printf("\n%s\n", pass ? "PASS: no deadlock, no leak, shared map works"
                         : "ISSUES FOUND (see above)");
    return pass ? 0 : 1;
}
