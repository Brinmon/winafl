/* shm.h -- shared sections used by the fork-server (P2.2).
 *
 * Two mappings:
 *   cov  - AFL's coverage map. We do NOT create it: afl-fuzz already creates
 *          `afl_shm_<id>` and maps it as trace_bits. The harness opens the same
 *          name and writes into it from clones. Shared sections survive
 *          RtlCloneUserProcess's copy-on-write, which fork-poc verified.
 *   ctl  - our own control block `afl_ctl_<id>`: outcome/exception/iteration.
 *
 * ASCII only.
 */
#ifndef FORK_SHM_H
#define FORK_SHM_H

#include <windows.h>
#include <stdint.h>

#include "fork_proto.h"

typedef struct ForkShm {
    HANDLE   cov_handle;
    uint8_t *cov;          /* MAP_SIZE bytes, shared with afl-fuzz */
    HANDLE   ctl_handle;
    ForkCtl *ctl;          /* our control block, shared with clones */
    char     cov_name[128];
    char     ctl_name[128];
} ForkShm;

/* Open the coverage map that afl-fuzz created. Returns 0 on success. */
int fork_shm_open_cov(ForkShm *s, const char *fuzzer_id, uint32_t map_size);

/* Create/open our control block (created by the harness, inherited by clones). */
int fork_shm_open_ctl(ForkShm *s, const char *fuzzer_id);

/* Convenience: both, given the fuzzer id from the environment. */
int fork_shm_open(ForkShm *s, const char *fuzzer_id, uint32_t map_size);

void fork_shm_close(ForkShm *s);

/* Reset the control block for one test case. */
void fork_shm_reset_ctl(ForkShm *s);

/* Record an outcome from inside a clone (safe: shared section, not CoW). */
void fork_shm_set_outcome(ForkShm *s, uint32_t outcome, uint32_t exception);

/* Increment the completed-iteration counter (parent side). */
void fork_shm_bump_iteration(ForkShm *s);

#endif /* FORK_SHM_H */
