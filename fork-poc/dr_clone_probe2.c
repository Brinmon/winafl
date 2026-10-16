/* dr_clone_probe2.c -- A1-v2: pin down DR + RtlCloneUserProcess.
 *
 * dr_clone_probe.exe established that DR reports:
 *   "Failed to follow into child process: Child thread not created suspended."
 *
 * DR wants the child's initial thread created SUSPENDED, and
 * RtlCloneUserProcess has exactly that flag (0x1). This probe runs the same
 * clone loop both ways so the difference is measured, not assumed:
 *
 *   dr_clone_probe2.exe <iters> <timeout_ms> normal     -- INHERIT_HANDLES only
 *   dr_clone_probe2.exe <iters> <timeout_ms> suspend    -- CREATE_SUSPENDED | INHERIT_HANDLES
 *
 * Every stage is logged to a file (shared handle survives the clone) so we can
 * tell parent and clone apart by PID.
 *
 * ASCII only.
 */
#include <windows.h>
#include <psapi.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#pragma comment(lib, "psapi.lib")

typedef LONG NTSTATUS;
#define STATUS_SUCCESS        ((NTSTATUS)0x00000000L)
#define STATUS_PROCESS_CLONED ((NTSTATUS)0x00000129L)

#define RTL_CLONE_CREATE_SUSPENDED 0x00000001u
#define RTL_CLONE_INHERIT_HANDLES  0x00000002u

typedef struct {
    ULONG  Length;
    HANDLE ProcessHandle;
    HANDLE ThreadHandle;
    BYTE   Reserved[512];
} CLONE_INFO;

typedef NTSTATUS(NTAPI *PFN_CLONE)(ULONG, PSECURITY_DESCRIPTOR,
                                   PSECURITY_DESCRIPTOR, HANDLE, CLONE_INFO *);

static PFN_CLONE g_clone;
static FILE     *g_log;

static void L(const char *fmt, ...)
{
    va_list ap;
    char buf[512];
    va_start(ap, fmt);
    _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    if (g_log) { fprintf(g_log, "[%lu] %s\n", (unsigned long)GetCurrentProcessId(), buf); fflush(g_log); }
    printf("[%lu] %s\n", (unsigned long)GetCurrentProcessId(), buf);
    fflush(stdout);
}

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

int main(int argc, char **argv)
{
    int iterations = (argc > 1) ? atoi(argv[1]) : 5;
    int timeout_ms = (argc > 2) ? atoi(argv[2]) : 3000;
    int use_suspend = (argc > 3 && strcmp(argv[3], "suspend") == 0);
    ULONG flags = RTL_CLONE_INHERIT_HANDLES;

    int cloned_ok = 0, failed = 0, timeouts = 0, created = 0;
    int i;
    volatile int sink = 0;
    DWORD t0, dt;

    setvbuf(stdout, NULL, _IONBF, 0);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);

    g_log = fopen("probe2.log", "a");
    if (g_log) { fprintf(g_log, "\n=== run: mode=%s iters=%d timeout=%d ===\n",
                         use_suspend ? "suspend" : "normal", iterations, timeout_ms);
                 fflush(g_log); }

    if (use_suspend) flags |= RTL_CLONE_CREATE_SUSPENDED;

    g_clone = (PFN_CLONE)GetProcAddress(GetModuleHandleA("ntdll.dll"), "RtlCloneUserProcess");
    if (!g_clone) { L("FATAL: no RtlCloneUserProcess"); return 3; }

    L("mode=%s flags=0x%lX", use_suspend ? "suspend" : "normal", (unsigned long)flags);

    for (i = 0; i < 16; i++) sink += probe_target(i);
    L("warmup done (sink=%d)", (int)sink);

    t0 = GetTickCount();

    for (i = 0; i < iterations; i++) {
        CLONE_INFO info;
        NTSTATUS st;
        DWORD w, exit_code = 0;

        memset(&info, 0, sizeof(info));
        info.Length = sizeof(info);

        L("iter %d: calling RtlCloneUserProcess", i);
        st = g_clone(flags, NULL, NULL, NULL, &info);
        L("iter %d: returned 0x%08lX (proc=%p thread=%p)",
          i, (unsigned long)st, (void *)info.ProcessHandle, (void *)info.ThreadHandle);

        if (st == STATUS_PROCESS_CLONED) {
            /* ---------------- CLONE ---------------- */
            L("CLONE: branch taken, calling instrumented target");
            sink += probe_target(i);
            L("CLONE: instrumented target returned, exiting");
            ExitProcess(0);
        }

        if (st != STATUS_SUCCESS) {
            failed++;
            if (failed >= 3) break;
            continue;
        }

        created++;

        /* With CREATE_SUSPENDED the parent owns the clone's initial thread and
         * must resume it, otherwise the clone never runs and we would time out
         * for a reason that has nothing to do with DR. */
        if (use_suspend && info.ThreadHandle) {
            DWORD prev = ResumeThread(info.ThreadHandle);
            L("PARENT: resumed clone thread (prev suspend count = %ld)", (long)prev);
        }

        w = WaitForSingleObject(info.ProcessHandle, (DWORD)timeout_ms);
        GetExitCodeProcess(info.ProcessHandle, &exit_code);
        if (info.ThreadHandle) CloseHandle(info.ThreadHandle);
        if (info.ProcessHandle) CloseHandle(info.ProcessHandle);

        if (w == WAIT_TIMEOUT) { timeouts++; L("PARENT: clone timed out"); }
        else if (exit_code == 0) cloned_ok++;
        else L("PARENT: clone exited 0x%08lX", (unsigned long)exit_code);
    }

    dt = GetTickCount() - t0;
    sink += probe_target(999);   /* parent can still run instrumented code */

    L("== results ==");
    L("  created   : %d", created);
    L("  ok (exit0): %d", cloned_ok);
    L("  timeouts  : %d", timeouts);
    L("  failed    : %d", failed);
    L("  elapsed   : %lums", (unsigned long)dt);
    L("  parent still runs instrumented code: yes (sink=%d)", (int)sink);

    if (g_log) fclose(g_log);
    printf("RESULT: %s\n", (created && cloned_ok == created) ? "PASS" : "FAIL");
    return (created && cloned_ok == created) ? 0 : 1;
}
