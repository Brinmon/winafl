/* forkloop.c -- the harness-side forkserver main loop (P2).
 *
 * Flow (matches afl-fuzz's existing run_target(), so the fuzzer side needs no
 * protocol change; see fork_proto.h):
 *
 *   connect pipe
 *   open shared sections (coverage + control)
 *   setup()            <- harness loads the closed-source DLL and warms it up
 *   >>> SNAPSHOT POINT <<<   (no locks held from here on)
 *   loop:
 *     send 'P' to afl-fuzz   (ready for a test case; afl-fuzz is blocked on this)
 *     wait for 'F'           (afl-fuzz has written the input file)
 *     clone
 *       child : fuzz_one(input) -> record outcome -> exit(0)
 *       parent: wait -> classify -> reply 'K' or 'C'+code
 *     after N clones: exit cleanly so afl-fuzz recreates a fresh process (P2.6)
 *
 * ASCII only.
 */
#include "clone.h"
#include "pipe.h"
#include "shm.h"

#include "fork_harness.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* afl-fuzz's map size; kept in sync with config.h (MAP_SIZE_POW2 16). */
#define FORK_MAP_SIZE 65536u

/* Default: retire the forkserver process every N clones (P2.6, decision D5).
 * Guards against any slow accumulation in the parent -- leaked handles, library
 * caches, heap fragmentation -- the same idea TinyInst uses with its iteration
 * counter. */
#define FORK_DEFAULT_RESTART_AFTER 100000u

static const char *default_input_file = ".cur_input";

/* Verbose tracing is opt-in: it costs a write+flush per clone and would distort
 * the throughput numbers we care about. Set WINAFL_FORK_DEBUG=1 to enable. */
static int g_debug;

static void dbg(const char *fmt, ...)
{
    va_list ap;
    if (!g_debug) return;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fflush(stderr);
}

static void fatal(const char *what, DWORD err)
{
    fprintf(stderr, "[fork-server] FATAL: %s (GLE=%lu)\n", what, (unsigned long)err);
    fflush(stderr);
    ExitProcess(3);
}

static uint32_t env_u32(const char *name, uint32_t dflt)
{
    char buf[64];
    DWORD n = GetEnvironmentVariableA(name, buf, sizeof(buf));
    if (n == 0 || n >= sizeof(buf)) return dflt;
    return (uint32_t)strtoul(buf, NULL, 10);
}

static void env_enable(const char *name)
{
    char buf[8];
    DWORD n = GetEnvironmentVariableA(name, buf, sizeof(buf));
    if (n > 0 && n < sizeof(buf) && buf[0] != '0') g_debug = 1;
}

/* Retire the process after `restart_after` clones (P2.6).
 *
 * We deliberately do NOT CreateProcess a replacement: afl-fuzz's run_target()
 * already re-creates the target whenever the child is gone
 * (`if (!is_child_running()) { destroy_target_process(0); create_target_process(argv); }`),
 * and its named pipe is a single instance, so a self-restarting forkserver would
 * race the fuzzer for the same pipe. Exiting cleanly hands the restart to the
 * fuzzer -- the same contract TinyInst uses (dr_exit_process after
 * fuzz_iterations). The next run starts from a fresh, un-polluted process. */
static void retire_process(uint32_t clones)
{
    if (g_debug) {
        fprintf(stderr, "[fork-server] retiring after %lu clones (fuzzer recreates)\n",
                (unsigned long)clones);
    }
    ExitProcess(0);
}

int fork_harness_main(ForkSetupFn setup, ForkFuzzFn fuzz_one)
{
    ForkPipe pipe;
    ForkShm shm;
    const char *id;
    const char *input_file = default_input_file;
    uint32_t timeout_ms, restart_after, restarts, done_this_process = 0;

    /* No interactive error dialogs: nobody is watching an unattended fuzzer. */
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    env_enable("WINAFL_FORK_DEBUG");

    if (!fork_clone_available()) {
        fprintf(stderr, "[fork-server] RtlCloneUserProcess unavailable (need Win10 1809+)\n");
        return 4;
    }

    id = getenv(FORK_ENV_ID);
    if (!id || !*id) {
        fprintf(stderr, "[fork-server] %s is not set (afl-fuzz must launch the harness)\n",
                FORK_ENV_ID);
        return 5;
    }
    timeout_ms = env_u32(FORK_ENV_TIMEOUT, 5000);
    restart_after = env_u32("WINAFL_FORK_RESTART_AFTER", FORK_DEFAULT_RESTART_AFTER);
    restarts = env_u32("WINAFL_FORK_RESTARTS", 0);

    if (fork_pipe_connect(&pipe, id, 20000) != 0) return 6;
    dbg("[fork-server] stage: pipe connected\n");

    if (fork_shm_open(&shm, id, FORK_MAP_SIZE) != 0) return 7;
    shm.ctl->restarts = restarts;
    dbg("[fork-server] stage: shm open (cov=%s ctl=%s)\n", shm.cov_name, shm.ctl_name);

    /* ---- everything the target needs must be ready BEFORE the snapshot ---- */
    if (setup) setup();
    dbg("[fork-server] stage: setup done, snapshot point reached\n");
    fflush(stdout);
    fflush(stderr);

    for (;;) {
        char go;
        ForkCloneInfo info;
        NTSTATUS st;
        DWORD exit_code = 0;
        int timed_out;

        /* ---- protocol, matching afl-fuzz's existing run_target() ---- */
        fork_shm_reset_ctl(&shm);
        fork_pipe_write_cmd(&pipe, FORK_CMD_PREPARE);   /* 'P': we are ready */
        go = fork_pipe_read_cmd(&pipe);                 /* block for 'F' or 'Q' */
        if (go == 0 || go == FORK_CMD_QUIT) break;
        if (go != FORK_CMD_FUZZING) {
            fprintf(stderr, "[fork-server] expected 'F', got '%c' (0x%02X)\n",
                    go, (unsigned char)go);
            break;
        }

        st = fork_clone_self(&info);

        if (st == STATUS_PROCESS_CLONED) {
            /* ---------------- CHILD ---------------- */
            /* Clean state: everything we write privately dies with us. */
            fuzz_one(input_file);
            fork_shm_set_outcome(&shm, FORK_OUTCOME_OK, 0);
            ExitProcess(0);
        }

        if (st != STATUS_SUCCESS) {
            /* Clone failed: report as a crash so the fuzzer does not silently spin. */
            fprintf(stderr, "[fork-server] clone failed: 0x%08lX\n", (unsigned long)st);
            fork_pipe_write_crash(&pipe, (DWORD)st);
            continue;
        }

        /* ---------------- PARENT ---------------- */
        timed_out = fork_clone_wait(info.ProcessHandle, timeout_ms, &exit_code);
        fork_clone_close(&info);

        if (timed_out) {
            fork_shm_set_outcome(&shm, FORK_OUTCOME_TIMEOUT, 0);
            fork_pipe_write_cmd(&pipe, FORK_CMD_DONE);  /* timeout == no new coverage */
        } else {
            /* The clone records the outcome in the shared block; trust it when
             * present, otherwise fall back to classifying the exit code. */
            uint32_t outcome = shm.ctl->outcome;
            uint32_t exception = shm.ctl->exception;
            if (outcome == FORK_OUTCOME_NONE)
                outcome = fork_clone_classify(exit_code, &exception);

            if (outcome == FORK_OUTCOME_CRASH) {
                fork_pipe_write_crash(&pipe, exception ? exception : exit_code);
            } else {
                fork_pipe_write_cmd(&pipe, FORK_CMD_DONE);
            }
        }
        fork_shm_bump_iteration(&shm);
        done_this_process++;

        if (restart_after && done_this_process >= restart_after)
            retire_process(done_this_process);   /* not reached */
    }

    fork_pipe_close(&pipe);
    fork_shm_close(&shm);
    return 0;
}
