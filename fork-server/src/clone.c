/* clone.c -- RtlCloneUserProcess wrapper (P2.4). ASCII only. */
#include "clone.h"

#include "fork_proto.h"

#include <stdio.h>
#include <string.h>

typedef NTSTATUS(NTAPI *PFN_RtlCloneUserProcess)(ULONG ProcessFlags,
                                                 PSECURITY_DESCRIPTOR ProcessSecurityDescriptor,
                                                 PSECURITY_DESCRIPTOR ThreadSecurityDescriptor,
                                                 HANDLE DebugPort,
                                                 ForkCloneInfo *ProcessInformation);

static PFN_RtlCloneUserProcess g_clone;
static int g_probed;

int fork_clone_available(void)
{
    if (!g_probed) {
        g_clone = (PFN_RtlCloneUserProcess)GetProcAddress(GetModuleHandleA("ntdll.dll"),
                                                          "RtlCloneUserProcess");
        g_probed = 1;
    }
    return g_clone != NULL;
}

NTSTATUS fork_clone_self(ForkCloneInfo *info)
{
    if (!fork_clone_available()) return (NTSTATUS)0xC0000139L; /* ENTRYPOINT_NOT_FOUND */
    memset(info, 0, sizeof(*info));
    info->Length = sizeof(*info);
    /* Deliberately NOT passing NO_SYNCHRONIZE: the default path drains the thread
     * pool and takes ntdll's locks, which is what makes cloning safe here. */
    return g_clone(FORK_CLONE_FLAG_INHERIT_HANDLES, NULL, NULL, NULL, info);
}

int fork_clone_wait(HANDLE process, uint32_t timeout_ms, DWORD *exit_code)
{
    DWORD w = WaitForSingleObject(process, timeout_ms);
    if (w == WAIT_TIMEOUT) {
        TerminateProcess(process, 0xDEADu);
        WaitForSingleObject(process, 5000);
        if (exit_code) *exit_code = 0xDEADu;
        return 1;
    }
    if (exit_code) {
        DWORD code = 0;
        GetExitCodeProcess(process, &code);
        *exit_code = code;
    }
    return 0;
}

void fork_clone_close(ForkCloneInfo *info)
{
    if (info->ThreadHandle) CloseHandle(info->ThreadHandle);
    if (info->ProcessHandle) CloseHandle(info->ProcessHandle);
    info->ThreadHandle = NULL;
    info->ProcessHandle = NULL;
}

uint32_t fork_clone_classify(DWORD exit_code, DWORD *exception_out)
{
    if (exception_out) *exception_out = 0;

    /* Clean exit. */
    if (exit_code == 0) return FORK_OUTCOME_OK;

    /* Our own timeout marker. */
    if (exit_code == 0xDEADu) return FORK_OUTCOME_TIMEOUT;

    /* NTSTATUS-style codes look like 0xC0000xxx / 0xE06D7363 etc. A process that
     * dies from an unhandled exception exits with the exception code, so anything
     * in the "error" range is reported as a crash with that code. */
    if ((exit_code & 0xF0000000u) == 0xC0000000u ||
        (exit_code & 0xF0000000u) == 0xE0000000u) {
        if (exception_out) *exception_out = exit_code;
        return FORK_OUTCOME_CRASH;
    }

    /* Non-zero but not an NTSTATUS (e.g. exit(1) from the harness): still a
     * deviation worth surfacing, but not necessarily a memory-safety crash. */
    if (exception_out) *exception_out = exit_code;
    return FORK_OUTCOME_CRASH;
}
