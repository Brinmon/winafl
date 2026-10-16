/* fork_harness.h -- the only API a fuzzing harness author needs.
 *
 * Usage (see fork-server/harness-template/harness.c):
 *
 *   static void fuzz_one(const char *input_file) { ...parse the file... }
 *   FORK_HARNESS(fuzz_one)              // expands to main()
 *
 * Everything else (loading the closed-source DLL, the snapshot point, cloning,
 * the pipe protocol) is handled by the fork-server library.
 */
#ifndef FORK_HARNESS_H
#define FORK_HARNESS_H

#include "fork_proto.h"

/* Called once, before the snapshot point: load the closed-source DLL, resolve
 * the parse entry point, and warm the library up so that no internal locks are
 * held when the first clone is made. Must be provided by the harness. */
typedef void (*ForkSetupFn)(void);

/* Called once per test case inside a fresh clone. Must return normally. */
typedef void (*ForkFuzzFn)(const char *input_file);

/* Entry point implemented by the library. */
int fork_harness_main(ForkSetupFn setup, ForkFuzzFn fuzz_one);

/* Convenience macro: define setup + fuzz_one, get a main(). */
#define FORK_HARNESS(setup_fn, fuzz_fn)          \
    int main(void)                               \
    {                                            \
        return fork_harness_main((setup_fn), (fuzz_fn)); \
    }

#endif /* FORK_HARNESS_H */
