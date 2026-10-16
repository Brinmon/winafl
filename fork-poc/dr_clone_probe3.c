/* dr_clone_probe3.c -- decisive test: clone + REAL libvips parse under TinyInst.
 *
 * dr_clone_probe2.exe proved TinyInst tolerates RtlCloneUserProcess on a toy
 * target. This probe tests the real scenario: a heavy closed-source DLL
 * (libvips-42.dll, 20MB) with deep, data-dependent control flow -- exactly the
 * case where TinyInst's lazily-translated indirect targets would need the
 * debugger that a clone does not have.
 *
 * Run under litecov (TinyInst):
 *   litecov.exe -- dr_clone_probe3.exe <iters> <seed.png>
 *
 * ASCII only.
 */
#include <windows.h>
#include <psapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef LONG NTSTATUS;
#define STATUS_SUCCESS        ((NTSTATUS)0x00000000L)
#define STATUS_PROCESS_CLONED ((NTSTATUS)0x00000129L)

typedef struct { ULONG Length; HANDLE P; HANDLE T; BYTE R[512]; } CI;
typedef NTSTATUS(NTAPI *PFN)(ULONG, PSECURITY_DESCRIPTOR, PSECURITY_DESCRIPTOR, HANDLE, CI *);

typedef void *(*PFN_parse)(const void *, size_t, const char *, ...);
typedef void  (*PFN_init)(const char *);

static PFN  g_clone;
static PFN_parse g_parse;

/* Non-trivial work inside the clone: parse a real image through libvips. */
__declspec(noinline) static int parse_in_clone(const unsigned char *buf, size_t len)
{
    void *img = g_parse(buf, len, "", NULL);
    return img ? 1 : 0;
}

int main(int argc, char **argv)
{
    int iterations = (argc > 1) ? atoi(argv[1]) : 20;
    const char *seed = (argc > 2) ? argv[2] : "seed.png";

    static unsigned char buf[1 << 20];
    size_t len = 0;
    FILE *f;
    HMODULE hv;
    int i, created = 0, ok = 0, timeouts = 0, other = 0;
    volatile int sink = 0;
    DWORD t0, dt;

    setvbuf(stdout, NULL, _IONBF, 0);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);

    hv = LoadLibraryA("libvips-42.dll");
    if (!hv) { printf("FATAL: libvips-42.dll not loaded (%lu)\n", GetLastError()); return 3; }
    g_parse = (PFN_parse)GetProcAddress(hv, "vips_image_new_from_buffer");
    if (!g_parse) { printf("FATAL: no vips_image_new_from_buffer\n"); return 4; }
    { PFN_init vi = (PFN_init)GetProcAddress(hv, "vips_init"); if (vi) vi("fork3"); }

    g_clone = (PFN)GetProcAddress(GetModuleHandleA("ntdll.dll"), "RtlCloneUserProcess");
    if (!g_clone) { printf("FATAL: no RtlCloneUserProcess\n"); return 5; }

    f = fopen(seed, "rb");
    if (!f) { printf("FATAL: cannot open seed '%s'\n", seed); return 6; }
    len = fread(buf, 1, sizeof(buf), f);
    fclose(f);
    printf("seed=%s (%zu bytes) iters=%d\n", seed, len, iterations);

    /* Warmup BEFORE the snapshot point: get libvips + its loaders initialised so
     * the common code paths are translated while the debugger is still alive. */
    for (i = 0; i < 8; i++) sink += parse_in_clone(buf, len);
    printf("warmup done (sink=%d)\n", (int)sink);

    t0 = GetTickCount();

    for (i = 0; i < iterations; i++) {
        CI ci;
        NTSTATUS st;
        DWORD w, ec = 0;

        memset(&ci, 0, sizeof(ci));
        ci.Length = sizeof(ci);
        st = g_clone(0x2, NULL, NULL, NULL, &ci);   /* INHERIT_HANDLES */

        if (st == STATUS_PROCESS_CLONED) {
            /* CLONE: do a real parse, then exit with a marker code so the parent
             * can distinguish "parse ran" from "crashed before/during parse". */
            int r = parse_in_clone(buf, len);
            ExitProcess(r ? 0x600D : 0x6000);
        }
        if (st != STATUS_SUCCESS) { printf("clone call failed 0x%08lX\n", (unsigned long)st); break; }

        created++;
        w = WaitForSingleObject(ci.P, 8000);
        GetExitCodeProcess(ci.P, &ec);
        if (ci.T) CloseHandle(ci.T);
        if (ci.P) CloseHandle(ci.P);

        if (w == WAIT_TIMEOUT)      { timeouts++; printf("  clone #%d TIMEOUT\n", i); }
        else if (ec == 0x600D)      { ok++; }
        else                        { other++; printf("  clone #%d bad exit 0x%08lX\n", i, (unsigned long)ec); }

        if ((i + 1) % 10 == 0)
            printf("  iter %3d: ok=%d bad=%d timeout=%d\n", i + 1, ok, other, timeouts);
    }

    dt = GetTickCount() - t0;
    sink += parse_in_clone(buf, len);   /* parent still works */

    printf("\n== results ==\n");
    printf("  created  : %d\n", created);
    printf("  ok(parse): %d\n", ok);
    printf("  bad exit : %d\n", other);
    printf("  timeouts : %d\n", timeouts);
    printf("  elapsed  : %lums (%.0f clones/sec)\n",
           (unsigned long)dt, created * 1000.0 / (dt ? dt : 1));
    printf("  parent still ok: yes (sink=%d)\n", (int)sink);
    printf("RESULT: %s\n", (created && ok == created) ? "PASS" :
                           (created ? "PARTIAL" : "FAIL"));
    return (created && ok == created) ? 0 : 1;
}
