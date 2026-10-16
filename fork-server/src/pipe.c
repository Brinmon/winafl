/* pipe.c -- named pipe client (P2.3). ASCII only. */
#include "pipe.h"

#include "fork_proto.h"

#include <stdio.h>
#include <string.h>

int fork_pipe_connect(ForkPipe *p, const char *fuzzer_id, uint32_t timeout_ms)
{
    memset(p, 0, sizeof(*p));
    _snprintf_s(p->name, sizeof(p->name), _TRUNCATE, FORK_PIPE_NAME_FMT, fuzzer_id);

    DWORD deadline = GetTickCount() + timeout_ms;
    for (;;) {
        HANDLE h = CreateFileA(p->name,
                               GENERIC_READ | GENERIC_WRITE,
                               0, NULL, OPEN_EXISTING, 0, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            /* afl-fuzz's pipe is message-oriented with 512-byte buffers; switch
             * our end to byte mode so single-byte commands arrive immediately. */
            DWORD mode = PIPE_READMODE_BYTE | PIPE_WAIT;
            SetNamedPipeHandleState(h, &mode, NULL, NULL);
            p->h = h;
            return 0;
        }
        if (GetLastError() != ERROR_PIPE_BUSY) {
            fprintf(stderr, "[fork-server] CreateFile(%s) failed (GLE=%lu)\n",
                    p->name, (unsigned long)GetLastError());
            return 1;
        }
        if (GetTickCount() > deadline) {
            fprintf(stderr, "[fork-server] pipe '%s' busy, gave up\n", p->name);
            return 1;
        }
        WaitNamedPipeA(p->name, 100);
    }
}

void fork_pipe_close(ForkPipe *p)
{
    if (p->h && p->h != INVALID_HANDLE_VALUE) {
        CloseHandle(p->h);
        p->h = NULL;
    }
}

char fork_pipe_read_cmd(ForkPipe *p)
{
    char c = 0;
    DWORD n = 0;
    if (!p->h) return 0;
    if (!ReadFile(p->h, &c, 1, &n, NULL) || n != 1) return 0;
    return c;
}

int fork_pipe_read_dword(ForkPipe *p, DWORD *out, uint32_t timeout_ms)
{
    /* afl-fuzz writes the exception code right after 'C'. Use a bounded wait via
     * PeekNamedPipe so a misbehaving peer cannot hang the harness forever. */
    DWORD deadline = GetTickCount() + timeout_ms;
    for (;;) {
        DWORD avail = 0;
        if (PeekNamedPipe(p->h, NULL, 0, NULL, &avail, NULL) && avail >= sizeof(DWORD)) {
            DWORD v = 0, n = 0;
            if (ReadFile(p->h, &v, sizeof(DWORD), &n, NULL) && n == sizeof(DWORD)) {
                *out = v;
                return 0;
            }
            return 1;
        }
        if (GetTickCount() > deadline) return 1;
        Sleep(1);
    }
}

int fork_pipe_write_cmd(ForkPipe *p, char cmd)
{
    DWORD n = 0;
    if (!p->h) return 1;
    /* NO FlushFileBuffers here: on a named pipe it blocks until the *peer* has
     * read the data, so flushing on both sides deadlocks (each waits for the
     * other to read first). afl-fuzz's own WriteCommandToPipe is a plain
     * WriteFile for exactly this reason; byte-mode pipes deliver immediately. */
    if (!WriteFile(p->h, &cmd, 1, &n, NULL) || n != 1) return 1;
    return 0;
}

int fork_pipe_write_crash(ForkPipe *p, DWORD exception_code)
{
    DWORD n = 0;
    if (fork_pipe_write_cmd(p, FORK_CMD_CRASH) != 0) return 1;
    if (!WriteFile(p->h, &exception_code, sizeof(DWORD), &n, NULL) || n != sizeof(DWORD))
        return 1;
    return 0;
}
