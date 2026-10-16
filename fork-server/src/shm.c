/* shm.c -- shared section management (P2.2). ASCII only. */
#include "shm.h"

#include <stdio.h>
#include <string.h>

static void die(const char *what, DWORD err)
{
    fprintf(stderr, "[fork-server] FATAL: %s (GLE=%lu)\n", what, (unsigned long)err);
    fflush(stderr);
    ExitProcess(2);
}

int fork_shm_open_cov(ForkShm *s, const char *fuzzer_id, uint32_t map_size)
{
    /* NOTE: do NOT memset(s) here -- fork_shm_open_ctl() already filled the ctl
     * fields (it runs first) and wiping the struct would lose s->ctl/s->ctl_handle,
     * producing a NULL-dereference in the caller. Initialisation belongs to
     * fork_shm_open(). */
    s->cov_handle = NULL;
    s->cov = NULL;
    _snprintf_s(s->cov_name, sizeof(s->cov_name), _TRUNCATE, FORK_COV_NAME_FMT, fuzzer_id);

    /* afl-fuzz created it; we only open it. */
    HANDLE h = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, s->cov_name);
    if (h == NULL) {
        fprintf(stderr, "[fork-server] cannot open coverage map '%s' (GLE=%lu)\n",
                s->cov_name, (unsigned long)GetLastError());
        return 1;
    }
    uint8_t *p = (uint8_t *)MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, map_size);
    if (p == NULL) {
        CloseHandle(h);
        fprintf(stderr, "[fork-server] MapViewOfFile(cov) failed (GLE=%lu)\n",
                (unsigned long)GetLastError());
        return 1;
    }
    s->cov_handle = h;
    s->cov = p;
    return 0;
}

int fork_shm_open_ctl(ForkShm *s, const char *fuzzer_id)
{
    _snprintf_s(s->ctl_name, sizeof(s->ctl_name), _TRUNCATE, FORK_CTL_NAME_FMT, fuzzer_id);

    HANDLE h = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE,
                                  0, FORK_CTL_BYTES, s->ctl_name);
    if (h == NULL) die("CreateFileMapping(ctl)", GetLastError());

    ForkCtl *p = (ForkCtl *)MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, FORK_CTL_BYTES);
    if (p == NULL) die("MapViewOfFile(ctl)", GetLastError());

    s->ctl_handle = h;
    s->ctl = p;

    if (p->magic != FORK_PROTO_MAGIC || p->version != FORK_PROTO_VERSION) {
        /* First use (or stale mapping from a previous run): initialise. */
        p->magic = FORK_PROTO_MAGIC;
        p->version = FORK_PROTO_VERSION;
        p->outcome = FORK_OUTCOME_NONE;
        p->exception = 0;
        p->iteration = 0;
        p->restarts = 0;
        p->flags = 0;
    }
    return 0;
}

int fork_shm_open(ForkShm *s, const char *fuzzer_id, uint32_t map_size)
{
    /* Single place that zeroes the struct: the per-map helpers deliberately only
     * touch their own fields so they can be called in either order. */
    memset(s, 0, sizeof(*s));
    if (fork_shm_open_ctl(s, fuzzer_id) != 0) return 1;
    if (fork_shm_open_cov(s, fuzzer_id, map_size) != 0) {
        /* Coverage is mandatory: without it the fuzzer is blind. */
        return 1;
    }
    return 0;
}

void fork_shm_close(ForkShm *s)
{
    if (s->cov) { UnmapViewOfFile(s->cov); s->cov = NULL; }
    if (s->cov_handle) { CloseHandle(s->cov_handle); s->cov_handle = NULL; }
    if (s->ctl) { UnmapViewOfFile((LPVOID)s->ctl); s->ctl = NULL; }
    if (s->ctl_handle) { CloseHandle(s->ctl_handle); s->ctl_handle = NULL; }
}

void fork_shm_reset_ctl(ForkShm *s)
{
    if (!s->ctl) return;
    s->ctl->outcome = FORK_OUTCOME_NONE;
    s->ctl->exception = 0;
}

void fork_shm_set_outcome(ForkShm *s, uint32_t outcome, uint32_t exception)
{
    if (!s->ctl) return;
    s->ctl->exception = exception;
    s->ctl->outcome = outcome;   /* write outcome last: it is the "ready" flag */
}

void fork_shm_bump_iteration(ForkShm *s)
{
    if (!s->ctl) return;
    s->ctl->iteration = s->ctl->iteration + 1;
}
