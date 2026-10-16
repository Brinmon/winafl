/* clone.h -- RtlCloneUserProcess wrapper (P2.4).
 *
 * The primitive was validated in fork-poc: the clone gets a copy-on-write view of
 * the address space, so anything it writes to private memory is discarded when it
 * exits, while shared sections stay shared. ASCII only.
 */
#ifndef FORK_CLONE_H
#define FORK_CLONE_H

#include <windows.h>
#include <stdint.h>

typedef LONG NTSTATUS;

#ifndef STATUS_SUCCESS
#define STATUS_SUCCESS ((NTSTATUS)0x00000000L)
#endif
#ifndef STATUS_PROCESS_CLONED
#define STATUS_PROCESS_CLONED ((NTSTATUS)0x00000129L)
#endif

#define FORK_CLONE_FLAG_CREATE_SUSPENDED 0x00000001u
#define FORK_CLONE_FLAG_INHERIT_HANDLES  0x00000002u
#define FORK_CLONE_FLAG_NO_SYNCHRONIZE   0x00000004u

/* Only the fields we use; the real struct is larger. */
typedef struct ForkCloneInfo {
    ULONG  Length;
    HANDLE ProcessHandle;
    HANDLE ThreadHandle;
    BYTE   Reserved[512];
} ForkCloneInfo;

/* Returns 1 if RtlCloneUserProcess is available on this system. */
int fork_clone_available(void);

/* Clone the current process.
 * Returns STATUS_PROCESS_CLONED in the child, STATUS_SUCCESS in the parent,
 * or another NTSTATUS on failure. */
NTSTATUS fork_clone_self(ForkCloneInfo *info);

/* Wait for a clone to finish; fills *exit_code. Returns 0 on success,
 * 1 if the wait timed out (the clone is terminated in that case). */
int fork_clone_wait(HANDLE process, uint32_t timeout_ms, DWORD *exit_code);

void fork_clone_close(ForkCloneInfo *info);

/* Map an exit code / exception code to the fork protocol outcome. */
uint32_t fork_clone_classify(DWORD exit_code, DWORD *exception_out);

#endif /* FORK_CLONE_H */
