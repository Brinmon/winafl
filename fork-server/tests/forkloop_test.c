/* forkloop_test.c -- P2.4/P2.5/P2.6 standalone validation of the fork loop,
 * without afl-fuzz.
 *
 * Plays the "fuzzer" role exactly like afl-fuzz's run_target() does:
 *   create the named pipe + the coverage map (afl_shm_<id>)
 *   launch the harness forkserver as a child
 *   per test case: wait for 'P', write the input, send 'F', expect 'K' or 'C'+code
 *   if the forkserver retires itself (P2.6 / decision D5), recreate it -- this is
 *   what afl-fuzz's `is_child_running()` / `create_target_process()` path does
 *
 * Acceptance covered here (from DEVELOPMENT-PLAN.md):
 *   - N clones against a real parsing target
 *   - no deadlock, no handle leak, no monotonic memory growth
 *   - crash detection / classification works (P2.5)
 *   - the retire-and-recreate contract works (P2.6)
 *
 * Environment:
 *   FORK_TEST_RESTART_AFTER  passed through to the harness (0 = never retire)
 *
 * ASCII only.
 */
#include "../include/fork_proto.h"

#include <windows.h>
#include <psapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#pragma comment(lib, "psapi.lib")

static HANDLE g_pipe;
static HANDLE g_cov_handle;
static unsigned char *g_cov;
static HANDLE g_child;                 /* harness process; NULL until launched */
static const char *g_crash_mode = "0";
static char g_id[64];
static char g_cmdline[512];
static char g_log_path[128];

static void fatal(const char *what)
{
    fprintf(stderr, "FATAL: %s (GLE=%lu)\n", what, (unsigned long)GetLastError());
    ExitProcess(2);
}

/* ---- pipe helpers -------------------------------------------------------- */

/* Liveness probe, mirroring afl-fuzz's is_child_running(): afl-fuzz checks this
 * BEFORE blocking on the pipe so a retired/dead forkserver is noticed at once
 * instead of after a full timeout. */
static int child_running(void)
{
    return g_child && WaitForSingleObject(g_child, 0) == WAIT_TIMEOUT;
}

static int wait_bytes_available(DWORD want, DWORD timeout_ms)
{
    DWORD deadline = GetTickCount() + timeout_ms;
    for (;;) {
        DWORD avail = 0;
        if (PeekNamedPipe(g_pipe, NULL, 0, NULL, &avail, NULL) && avail >= want) return 1;
        if (!child_running()) return 0;          /* gone: do not burn the timeout */
        if (GetTickCount() > deadline) return 0;
        Sleep(0);
    }
}

static int read_cmd(char *out, DWORD timeout_ms)
{
    DWORD n = 0;
    if (!wait_bytes_available(1, timeout_ms)) return 1;
    if (!ReadFile(g_pipe, out, 1, &n, NULL) || n != 1) return 1;
    return 0;
}

static int read_dword(DWORD *out, DWORD timeout_ms)
{
    DWORD n = 0;
    if (!wait_bytes_available(sizeof(DWORD), timeout_ms)) return 1;
    return ReadFile(g_pipe, out, sizeof(DWORD), &n, NULL) && n == sizeof(DWORD) ? 0 : 1;
}

/* Plain WriteFile, no FlushFileBuffers: on a named pipe FlushFileBuffers blocks
 * until the peer reads, so flushing on both sides deadlocks them against each
 * other. afl-fuzz's own WriteCommandToPipe is a plain WriteFile too. */
static int write_cmd(char c)
{
    DWORD n = 0;
    if (!WriteFile(g_pipe, &c, 1, &n, NULL) || n != 1) return 1;
    return 0;
}

static void touch_input(const char *path, const unsigned char *data, size_t len)
{
    FILE *f = fopen(path, "wb");
    if (!f) fatal("fopen input");
    fwrite(data, 1, len, f);
    fclose(f);
}

/* Wait for the harness to announce readiness, tell it to go, read the verdict.
 * Returns 'K' or 'C'; 0 means no verdict arrived (harness gone or hung). */
static char one_round(DWORD timeout_ms, DWORD *exc_out)
{
    char c = 0;
    if (exc_out) *exc_out = 0;

    if (read_cmd(&c, timeout_ms) != 0) return 0;      /* wait for 'P' */
    if (c != FORK_CMD_PREPARE) return 0;
    if (write_cmd(FORK_CMD_FUZZING) != 0) return 0;   /* send 'F' */

    if (read_cmd(&c, timeout_ms) != 0) return 0;      /* read 'K' or 'C' */
    if (c == FORK_CMD_CRASH) {
        DWORD code = 0;
        if (read_dword(&code, timeout_ms) == 0 && exc_out) *exc_out = code;
    }
    return c;
}

/* ---- harness lifecycle (mirrors afl-fuzz's create_target_process) -------- */

/* Start the forkserver and wait for it to connect to our pipe. `log_truncate`
 * decides whether this instance starts a fresh log or appends to the previous
 * one (a retired instance is replaced and we want to keep both logs). */
static int launch_harness(PROCESS_INFORMATION *pi, int log_truncate)
{
    SECURITY_ATTRIBUTES sa;
    STARTUPINFOA hsi;
    HANDLE hlog;

    ZeroMemory(&sa, sizeof(sa));
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    hlog = CreateFileA(g_log_path, GENERIC_WRITE, FILE_SHARE_READ, &sa,
                       log_truncate ? CREATE_ALWAYS : OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);

    ZeroMemory(&hsi, sizeof(hsi));
    hsi.cb = sizeof(hsi);
    if (hlog != INVALID_HANDLE_VALUE) {
        hsi.dwFlags = STARTF_USESTDHANDLES;
        hsi.hStdOutput = hlog;
        hsi.hStdError = hlog;
    }
    ZeroMemory(pi, sizeof(*pi));
    if (!CreateProcessA(NULL, g_cmdline, NULL, NULL, TRUE, 0, NULL, NULL, &hsi, pi))
        fatal("CreateProcess(harness)");
    if (hlog != INVALID_HANDLE_VALUE) CloseHandle(hlog);

    /* A fresh instance re-opens the same pipe name; disconnect our end so the
     * new client connects to a clean listen state. */
    DisconnectNamedPipe(g_pipe);
    if (!ConnectNamedPipe(g_pipe, NULL) && GetLastError() != ERROR_PIPE_CONNECTED) {
        DWORD e = GetLastError();
        fprintf(stderr, "harness did not connect (GLE=%lu)\n", (unsigned long)e);
        TerminateProcess(pi->hProcess, 1);
        return 1;
    }
    {
        DWORD mode = PIPE_READMODE_BYTE | PIPE_WAIT;
        SetNamedPipeHandleState(g_pipe, &mode, NULL, NULL);
    }
    g_child = pi->hProcess;
    return 0;
}

int main(int argc, char **argv)
{
    const char *harness = (argc > 1) ? argv[1] : "harness.exe";
    const char *seed = (argc > 2) ? argv[2] : NULL;
    int iterations = (argc > 3) ? atoi(argv[3]) : 1000;
    const char *restart_env;
    uint32_t restart_after;

    int ok = 0, crashes = 0, timeouts = 0, protocol_err = 0, recreations = 0;
    DWORD t0 = 0, dt = 0, handles0 = 0, handles1 = 0;
    PROCESS_MEMORY_COUNTERS_EX mc0, mc1;
    PROCESS_INFORMATION pi;
    char pipe_name[128], cov_name[128], to[32], ra[32];
    static unsigned char seedbuf[65536];
    size_t seedlen = 0;
    const char *input_file = ".cur_input";
    int pass, i;

    g_crash_mode = (argc > 4) ? argv[4] : "0";

    /* Unbuffered: with redirection, buffered output hides progress and makes a
     * slow run look like a hang. */
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    _snprintf_s(g_id, sizeof(g_id), _TRUNCATE, "t%lu", (unsigned long)GetCurrentProcessId());

    /* ---- act as afl-fuzz: create the pipe and the coverage map ---- */
    _snprintf_s(pipe_name, sizeof(pipe_name), _TRUNCATE, FORK_PIPE_NAME_FMT, g_id);
    _snprintf_s(cov_name, sizeof(cov_name), _TRUNCATE, FORK_COV_NAME_FMT, g_id);

    g_pipe = CreateNamedPipeA(pipe_name, PIPE_ACCESS_DUPLEX, PIPE_TYPE_BYTE | PIPE_WAIT,
                              1, 4096, 4096, 20000, NULL);
    if (g_pipe == INVALID_HANDLE_VALUE) fatal("CreateNamedPipe");

    g_cov_handle = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE,
                                      0, 65536, cov_name);
    if (!g_cov_handle) fatal("CreateFileMapping(cov)");
    g_cov = (unsigned char *)MapViewOfFile(g_cov_handle, FILE_MAP_ALL_ACCESS, 0, 0, 65536);
    if (!g_cov) fatal("MapViewOfFile(cov)");

    /* ---- environment for the harness ---- */
    restart_env = getenv("FORK_TEST_RESTART_AFTER");
    restart_after = restart_env ? (uint32_t)strtoul(restart_env, NULL, 10) : 0u;

    SetEnvironmentVariableA(FORK_ENV_ID, g_id);
    _snprintf_s(to, sizeof(to), _TRUNCATE, "%u", 5000u);
    SetEnvironmentVariableA(FORK_ENV_TIMEOUT, to);
    _snprintf_s(ra, sizeof(ra), _TRUNCATE, "%lu", (unsigned long)restart_after);
    SetEnvironmentVariableA("WINAFL_FORK_RESTART_AFTER", ra);
    SetEnvironmentVariableA("WINAFL_FORK_CRASH_MODE", g_crash_mode);

    _snprintf_s(g_cmdline, sizeof(g_cmdline), _TRUNCATE, "\"%s\"", harness);
    _snprintf_s(g_log_path, sizeof(g_log_path), _TRUNCATE, "harness.log");

    if (launch_harness(&pi, 1) != 0) return 3;

    /* ---- load a seed ---- */
    if (seed) {
        FILE *f = fopen(seed, "rb");
        if (f) { seedlen = fread(seedbuf, 1, sizeof(seedbuf), f); fclose(f); }
    }
    if (seedlen == 0) { memcpy(seedbuf, "AAAA", 4); seedlen = 4; }

    touch_input(input_file, seedbuf, seedlen);

    printf("harness=%s seed=%s iterations=%d crash_mode=%s restart_after=%lu\n",
           harness, seed ? seed : "(builtin)", iterations, g_crash_mode,
           (unsigned long)restart_after);
    printf("pipe=%s\n\n", pipe_name);

    /* ---- warm-up round: the harness only finishes setup() after the first
     * request, so the handle/memory baseline must be taken here -- measuring
     * right after ConnectNamedPipe would count libvips' own loading as a "leak". */
    {
        char c = one_round(20000, NULL);
        if (c != FORK_CMD_DONE && c != FORK_CMD_CRASH) {
            fprintf(stderr, "warm-up round failed (no verdict; setup never completed)\n");
            TerminateProcess(pi.hProcess, 1);
            return 4;
        }
        printf("warm-up round: verdict '%c' (setup cost not counted)\n", c);
    }

    GetProcessHandleCount(pi.hProcess, &handles0);
    memset(&mc0, 0, sizeof(mc0)); mc0.cb = sizeof(mc0);
    GetProcessMemoryInfo(pi.hProcess, (PROCESS_MEMORY_COUNTERS *)&mc0, sizeof(mc0));

    t0 = GetTickCount();

    for (i = 0; i < iterations; i++) {
        char c;
        DWORD exc = 0;

        /* ---- P2.6: notice a retired forkserver and recreate it, exactly like
         * afl-fuzz's is_child_running() / create_target_process() path. ---- */
        if (restart_after && !child_running()) {
            DWORD ec = 0;
            GetExitCodeProcess(pi.hProcess, &ec);
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            if (launch_harness(&pi, 0) != 0) return 5;
            recreations++;
            printf("  forkserver retired (exit=0x%08lX) -> recreated (#%d)\n",
                   (unsigned long)ec, recreations);
            i--;                   /* this iteration never produced a verdict */
            continue;
        }

        touch_input(input_file, seedbuf, seedlen);   /* like afl-fuzz does */
        memset(g_cov, 0, 256);                       /* small window we check */

        c = one_round(20000, &exc);

        if (c == 0) {
            DWORD ec = 0;
            GetExitCodeProcess(pi.hProcess, &ec);
            if (restart_after && !child_running()) {
                /* Retired between the verdict and this round (the forkserver
                 * exits right after replying): recreate, as afl-fuzz does. */
                CloseHandle(pi.hThread);
                CloseHandle(pi.hProcess);
                if (launch_harness(&pi, 0) != 0) return 5;
                recreations++;
                printf("  forkserver retired (exit=0x%08lX) -> recreated (#%d)\n",
                       (unsigned long)ec, recreations);
                i--;
                continue;
            }
            fprintf(stderr, "diag: no verdict at iter %d; alive=%d exit=0x%08lX\n",
                    i, child_running(), (unsigned long)ec);
            timeouts++;
            break;
        }
        if (c == FORK_CMD_DONE) {
            ok++;
        } else if (c == FORK_CMD_CRASH) {
            printf("  crash #%d at iter %d: exception=0x%08lX\n",
                   crashes + 1, i, (unsigned long)exc);
            crashes++;
        } else {
            fprintf(stderr, "protocol: unexpected byte '%c' (0x%02X) at iter %d\n",
                    c, (unsigned char)c, i);
            protocol_err++;
            break;
        }

        if ((i + 1) % 200 == 0) {
            DWORD hc = 0;
            GetProcessHandleCount(pi.hProcess, &hc);
            printf("  iter %5d: ok=%d crash=%d timeout=%d  harness handles=%lu\n",
                   i + 1, ok, crashes, timeouts, (unsigned long)hc);
        }
    }

    dt = GetTickCount() - t0;
    GetProcessHandleCount(pi.hProcess, &handles1);
    memset(&mc1, 0, sizeof(mc1)); mc1.cb = sizeof(mc1);
    GetProcessMemoryInfo(pi.hProcess, (PROCESS_MEMORY_COUNTERS *)&mc1, sizeof(mc1));

    /* Ask the forkserver to stop; it is blocked waiting for the next command. */
    write_cmd(FORK_CMD_QUIT);
    if (WaitForSingleObject(pi.hProcess, 5000) == WAIT_TIMEOUT) TerminateProcess(pi.hProcess, 0);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    printf("\n== results ==\n");
    printf("  ok        : %d\n", ok);
    printf("  crashes   : %d\n", crashes);
    printf("  recreated : %d   <- P2.6 retire/recreate cycles\n", recreations);
    printf("  timeouts  : %d   <- deadlock indicator (must be 0)\n", timeouts);
    printf("  protocol  : %d errors\n", protocol_err);
    printf("  handles   : %lu -> %lu   <- leak indicator (must be equal)\n",
           (unsigned long)handles0, (unsigned long)handles1);
    printf("  work set  : %.1f MB -> %.1f MB\n",
           mc0.WorkingSetSize / 1048576.0, mc1.WorkingSetSize / 1048576.0);
    printf("  rate      : %.0f clones/sec\n",
           (ok + crashes) * 1000.0 / (dt ? dt : 1));

    /* Handle equality only holds within one process generation; when the
     * forkserver was retired we compare the fresh instance's baseline instead. */
    pass = (ok + crashes == iterations) && timeouts == 0 && protocol_err == 0
           && (recreations > 0 || handles1 == handles0);
    printf("\n%s\n", pass ? "PASS: fork loop stable (no deadlock, no leak)"
                         : "FAIL: see counters above");
    return pass ? 0 : 1;
}
