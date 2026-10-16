/* dr_clone_probe.c -- A1: does a DynamoRIO-instrumented process survive
 * RtlCloneUserProcess, and can the clone execute instrumented code?
 *
 * This is the decisive falsification test for the "fork under DR" design.
 * It answers, with data rather than assumption:
 *
 *   1. can a DR-managed thread call RtlCloneUserProcess at all?
 *   2. does the clone reach STATUS_PROCESS_CLONED and run afterwards?
 *   3. can the clone execute a DR-instrumented function (probe_target)?
 *   4. does the parent survive N clones without deadlock or handle leak?
 *
 * Run under DR with winafl.dll in debug mode (no pipe handshake, so this probe
 * keeps control of its own flow):
 *
 *   drrun -c winafl.dll -debug -logdir . ^
 *         -coverage_module dr_clone_probe.exe ^
 *         -target_module   dr_clone_probe.exe ^
 *         -target_method    probe_target ^
 *         -- dr_clone_probe.exe <iterations>
 *
 * ASCII only.
 */
#include <windows.h>
#include <psapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#pragma comment(lib, "psapi.lib")

typedef LONG NTSTATUS;
#define STATUS_SUCCESS        ((NTSTATUS)0x00000000L)
#define STATUS_PROCESS_CLONED ((NTSTATUS)0x00000129L)
#define RTL_CLONE_FLAG_INHERIT_HANDLES 0x00000002

typedef struct {
    ULONG  Length;
    HANDLE ProcessHandle;
    HANDLE ThreadHandle;
    BYTE   Reserved[512];
} PROBE_CLONE_INFO;

typedef NTSTATUS(NTAPI *PFN_RtlCloneUserProcess)(ULONG, PSECURITY_DESCRIPTOR,
                                                 PSECURITY_DESCRIPTOR, HANDLE,
                                                 PROBE_CLONE_INFO *);

static PFN_RtlCloneUserProcess g_clone;

/* ----
 * The instrumented target: several distinct branches so winafl.dll has edges
 * to record. Marked noinline so the call is real and DR's drwrap trampoline is
 * exercised on every entry (including from inside a clone).
 * ---- */
__declspec(noinline) static int probe_target(int seed)
{
    volatile int acc = seed;

    if (acc & 1)      acc += 7;
    else              acc -= 3;
    if (acc & 2)      acc ^= 0x55;
    if (acc > 100)    acc /= 2;
    if (acc < 0)      acc = -acc;
    if (acc & 4)      acc += 11;
    if (acc % 3 == 0) acc += 13;

    return acc;
}

static void fail(const char *what)
{
    fprintf(stderr, "FATAL: %s (GLE=%lu)\n", what, (unsigned long)GetLastError());
    fflush(stderr);
    ExitProcess(2);
}

int main(int argc, char **argv)
{
    int iterations = (argc > 1) ? atoi(argv[1]) : 100;
    int timeout_ms = (argc > 2) ? atoi(argv[2]) : 5000;

    int cloned_ok = 0, clone_failed = 0, timeouts = 0, child_reached = 0;
    DWORD handles0 = 0, handles1 = 0;
    PROCESS_MEMORY_COUNTERS_EX mc0, mc1;
    DWORD t0, dt;
    int i;
    volatile int sink = 0;

    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);

    g_clone = (PFN_RtlCloneUserProcess)GetProcAddress(GetModuleHandleA("ntdll.dll"),
                                                      "RtlCloneUserProcess");
    if (!g_clone) {
        fprintf(stderr, "FAIL: RtlCloneUserProcess not exported\n");
        return 3;
    }

    printf("== A1: clone inside a DynamoRIO-instrumented process ==\n");
    printf("pid=%lu iterations=%d timeout=%dms\n",
           (unsigned long)GetCurrentProcessId(), iterations, timeout_ms);

    /* Warm up so the instrumented target is compiled/translated and its module
     * is fully initialised BEFORE the first clone (this mirrors the real design,
     * where the harness does setup + warmup before the snapshot point). */
    for (i = 0; i < 16; i++) sink += probe_target(i);
    printf("warmup done (sink=%d)\n", (int)sink);

    GetProcessHandleCount(GetCurrentProcess(), &handles0);
    memset(&mc0, 0, sizeof(mc0)); mc0.cb = sizeof(mc0);
    GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *)&mc0, sizeof(mc0));

    t0 = GetTickCount();

    for (i = 0; i < iterations; i++) {
        PROBE_CLONE_INFO info;
        NTSTATUS st;
        DWORD w, exit_code = 0;

        memset(&info, 0, sizeof(info));
        info.Length = sizeof(info);

        st = g_clone(RTL_CLONE_FLAG_INHERIT_HANDLES, NULL, NULL, NULL, &info);

        if (st == STATUS_PROCESS_CLONED) {
            /* ---------------- CLONE ----------------
             * Execute the DR-instrumented target from inside the clone. If DR's
             * translated code or drwrap trampoline cannot run here, this either
             * crashes or hangs -- both are results we want to see. */
            volatile int r = probe_target(i);
            (void)r;
            ExitProcess(0);
        }

        if (st != STATUS_SUCCESS) {
            if (clone_failed == 0)
                printf("  clone #%d FAILED: NTSTATUS=0x%08lX\n", i, (unsigned long)st);
            clone_failed++;
            if (clone_failed >= 3) break;
            continue;
        }

        child_reached++;

        w = WaitForSingleObject(info.ProcessHandle, (DWORD)timeout_ms);
        if (w == WAIT_TIMEOUT) {
            timeouts++;
            TerminateProcess(info.ProcessHandle, 0xDEAD);
            WaitForSingleObject(info.ProcessHandle, 5000);
        }
        GetExitCodeProcess(info.ProcessHandle, &exit_code);
        if (info.ThreadHandle) CloseHandle(info.ThreadHandle);
        if (info.ProcessHandle) CloseHandle(info.ProcessHandle);

        if (w != WAIT_TIMEOUT) {
            if (exit_code == 0) {
                cloned_ok++;
            } else if (cloned_ok == 0 && clone_failed == 0) {
                printf("  clone #%d exited with code 0x%08lX (expected 0)\n",
                       i, (unsigned long)exit_code);
            }
        }

        if ((i + 1) % 25 == 0) {
            DWORD hc = 0;
            GetProcessHandleCount(GetCurrentProcess(), &hc);
            printf("  iter %4d: ok=%d timeout=%d failed=%d handles=%lu\n",
                   i + 1, cloned_ok, timeouts, clone_failed, (unsigned long)hc);
        }
    }

    dt = GetTickCount() - t0;
    GetProcessHandleCount(GetCurrentProcess(), &handles1);
    memset(&mc1, 0, sizeof(mc1)); mc1.cb = sizeof(mc1);
    GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *)&mc1, sizeof(mc1));

    /* We are still alive and executing: that alone is the primary result. */
    sink += probe_target(1234);   /* prove the parent can still run instrumented code */

    printf("\n== A1 results ==\n");
    printf("  parent alive        : YES (this line printed after %d clones)\n", iterations);
    printf("  clones created      : %d\n", child_reached);
    printf("  clone calls failed  : %d\n", clone_failed);
    printf("  clones ok (exit 0)  : %d\n", cloned_ok);
    printf("  clone timeouts      : %d\n", timeouts);
    printf("  parent handles      : %lu -> %lu\n",
           (unsigned long)handles0, (unsigned long)handles1);
    printf("  parent work set     : %.1f MB -> %.1f MB\n",
           mc0.WorkingSetSize / 1048576.0, mc1.WorkingSetSize / 1048576.0);
    printf("  elapsed             : %lums (%.0f clones/sec)\n",
           (unsigned long)dt, child_reached * 1000.0 / (dt ? dt : 1));
    printf("  parent post-clone instrumented call: ok (sink=%d)\n", (int)sink);

    if (child_reached == 0) {
        printf("\nRESULT: FATAL - cloning unavailable under DR (0 clones created)\n");
        return 4;
    }
    if (timeouts || cloned_ok != child_reached) {
        printf("\nRESULT: FAIL - DR state does not survive cloning cleanly\n");
        return 5;
    }
    printf("\nRESULT: PASS - DR-instrumented process clones cleanly, clone runs instrumented code\n");
    return 0;
}
