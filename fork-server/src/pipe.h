/* pipe.h -- named pipe client used by the harness forkserver (P2.3).
 *
 * The pipe is created by afl-fuzz; we connect and speak the command protocol
 * defined in fork_proto.h. ASCII only.
 */
#ifndef FORK_PIPE_H
#define FORK_PIPE_H

#include <windows.h>
#include <stdint.h>

typedef struct ForkPipe {
    HANDLE h;
    char   name[128];
} ForkPipe;

/* Connect to afl-fuzz's pipe. Retries for `timeout_ms` because the fuzzer may
 * still be setting up. Returns 0 on success. */
int fork_pipe_connect(ForkPipe *p, const char *fuzzer_id, uint32_t timeout_ms);

void fork_pipe_close(ForkPipe *p);

/* Blocking read of one command byte. Returns 0 on EOF/error. */
char fork_pipe_read_cmd(ForkPipe *p);

/* Read a DWORD payload (used after 'C' to receive an exception code). */
int fork_pipe_read_dword(ForkPipe *p, DWORD *out, uint32_t timeout_ms);

int fork_pipe_write_cmd(ForkPipe *p, char cmd);

int fork_pipe_write_crash(ForkPipe *p, DWORD exception_code);

#endif /* FORK_PIPE_H */
