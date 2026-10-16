/* fork_proto.h -- shared protocol between afl-fuzz (fuzzer side) and the
 * fork-server embedded in a harness (target side).
 *
 * Design (see fork-poc/DEVELOPMENT-PLAN.md):
 *   - the harness main process is the FORKSERVER; it talks to afl-fuzz over the
 *     existing named pipe created by afl-fuzz (`\\.\pipe\afl_pipe_<id>`)
 *   - each test case runs in a CLONE produced by RtlCloneUserProcess; the clone
 *     writes its result into a shared control block (CoW does not affect shared
 *     sections) and exits
 *   - coverage is written by the clone straight into AFL's coverage map
 *     (`afl_shm_<id>`), which afl-fuzz already maps as `trace_bits`
 *
 * Command directions deliberately match what afl-fuzz already implements
 * (see run_target() in afl-fuzz.c and pre_fuzz_handler() in winafl.c), so the
 * fuzzer side needs NO protocol change:
 *
 *   harness -> fuzzer : 'P' ready to run (afl-fuzz blocks reading this)
 *   fuzzer  -> harness: 'F' go     (afl-fuzz sends this after reading 'P')
 *   harness -> fuzzer : 'K' clean exit
 *   harness -> fuzzer : 'C' crash, followed by a DWORD exception code
 *   fuzzer  -> harness: 'Q' quit   (clean shutdown request)
 *
 * Keep this file ASCII-only.
 */
#ifndef FORK_PROTO_H
#define FORK_PROTO_H

#include <stdint.h>

#define FORK_PROTO_MAGIC   0x4B524F46u /* 'FORK' */
#define FORK_PROTO_VERSION 2u

/* Names created by afl-fuzz; the harness only opens them. */
#define FORK_PIPE_NAME_FMT  "\\\\.\\pipe\\afl_pipe_%s"
#define FORK_COV_NAME_FMT   "afl_shm_%s"
#define FORK_CTL_NAME_FMT   "afl_ctl_%s"

/* Environment variable through which afl-fuzz tells the harness its id.
 * (Target argv must not be polluted: '@@' semantics rely on it.) */
#define FORK_ENV_ID         "WINAFL_FORK_ID"
#define FORK_ENV_TIMEOUT    "WINAFL_FORK_TIMEOUT"

/* Command bytes (must match afl-fuzz). */
#define FORK_CMD_PREPARE 'P'   /* harness -> fuzzer: ready for a test case */
#define FORK_CMD_FUZZING 'F'   /* fuzzer  -> harness: run it                 */
#define FORK_CMD_DONE    'K'   /* harness -> fuzzer: clean exit              */
#define FORK_CMD_CRASH   'C'   /* harness -> fuzzer: crash (+ DWORD code)    */
#define FORK_CMD_QUIT    'Q'   /* fuzzer  -> harness: quit                   */

/* Clone outcome, written by the clone into the control block. */
enum ForkOutcome {
    FORK_OUTCOME_NONE    = 0,
    FORK_OUTCOME_OK      = 1,  /* fuzz_one returned normally */
    FORK_OUTCOME_CRASH   = 2,  /* unhandled exception in the clone */
    FORK_OUTCOME_TIMEOUT = 3   /* parent gave up waiting */
};

/* Shared control block (created by the harness, opened by nothing else).
 * volatile: written by clones, read by the parent forkserver. */
typedef struct ForkCtl {
    uint32_t magic;
    uint32_t version;
    volatile uint32_t outcome;      /* enum ForkOutcome */
    volatile uint32_t exception;    /* exception code, e.g. 0xC0000005 */
    volatile uint32_t iteration;    /* total clones completed */
    volatile uint32_t restarts;     /* parent process restarts */
    volatile uint32_t flags;        /* reserved */
    volatile uint32_t pad[9];
} ForkCtl;

#define FORK_CTL_BYTES 4096

/* Compile-time guard so both sides cannot drift apart silently. */
typedef char fork_proto_size_check[(sizeof(ForkCtl) <= FORK_CTL_BYTES) ? 1 : -1];

#endif /* FORK_PROTO_H */
