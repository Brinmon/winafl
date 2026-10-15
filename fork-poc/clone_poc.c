// clone_poc.c -- P0: verify RtlCloneUserProcess works on this machine.
//
// Proves the core primitive we need for a Windows fork-server:
//   1. a parent process can clone itself (copy-on-write address space)
//   2. the clone runs from the same instruction pointer with the same memory
//   3. writes in the clone DO NOT affect the parent (isolation = clean state)
//
// Build: cl /nologo /O2 clone_poc.c /Fe:clone_poc.exe
#include <windows.h>
#include <stdio.h>
#include <string.h>

typedef LONG NTSTATUS;
#define STATUS_SUCCESS            ((NTSTATUS)0x00000000L)
#define STATUS_PROCESS_CLONED      ((NTSTATUS)0x00000129L)

#define RTL_CLONE_PROCESS_FLAGS_CREATE_SUSPENDED 0x00000001
#define RTL_CLONE_PROCESS_FLAGS_INHERIT_HANDLES  0x00000002
#define RTL_CLONE_PROCESS_FLAGS_NO_SYNCHRONIZE   0x00000004

typedef struct _RTL_USER_PROCESS_INFORMATION {
    ULONG Length;
    HANDLE Process;
    HANDLE Thread;
    /* ... remaining fields exist but we only need Process/Thread here */
    BYTE Reserved[512];
} RTL_USER_PROCESS_INFORMATION;

typedef NTSTATUS (NTAPI *PFN_RtlCloneUserProcess)(
    ULONG ProcessFlags,
    PSECURITY_DESCRIPTOR ProcessSecurityDescriptor,
    PSECURITY_DESCRIPTOR ThreadSecurityDescriptor,
    HANDLE DebugPort,
    RTL_USER_PROCESS_INFORMATION *ProcessInformation);

// Global state the clone must NOT be able to corrupt in the parent.
static volatile long g_counter = 0;
static char g_buffer[64] = "PARENT-ORIGINAL";

int main(void)
{
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    PFN_RtlCloneUserProcess pClone =
        (PFN_RtlCloneUserProcess)GetProcAddress(ntdll, "RtlCloneUserProcess");
    if (!pClone) {
        printf("FAIL: RtlCloneUserProcess not exported (need Win10 1809+)\n");
        return 2;
    }
    printf("OK: RtlCloneUserProcess found at %p\n", (void *)pClone);

    // Mutate state so we can tell parent and clone apart.
    g_counter = 1;
    strcpy_s(g_buffer, sizeof(g_buffer), "BEFORE-CLONE");

    RTL_USER_PROCESS_INFORMATION info;
    memset(&info, 0, sizeof(info));
    info.Length = sizeof(info);

    NTSTATUS st = pClone(RTL_CLONE_PROCESS_FLAGS_INHERIT_HANDLES,
                         NULL, NULL, NULL, &info);

    if (st == STATUS_PROCESS_CLONED) {
        // ---- We are the CLONE ----
        printf("  [clone ] pid=%lu  counter=%ld  buffer=%s\n",
               GetCurrentProcessId(), g_counter, g_buffer);
        // Trash everything. The parent must not see any of this.
        g_counter = 999;
        strcpy_s(g_buffer, sizeof(g_buffer), "CLONE-TRASHED-IT");
        printf("  [clone ] after tampering: counter=%ld buffer=%s\n",
               g_counter, g_buffer);
        printf("  [clone ] exiting (state is discarded)\n");
        return 0;
    }

    if (st == STATUS_SUCCESS) {
        // ---- We are the PARENT ----
        printf("OK: cloned child pid=%lu thread=%p\n",
               GetProcessId(info.Process), (void *)info.Thread);
        WaitForSingleObject(info.Process, 10000);
        DWORD code = 0;
        GetExitCodeProcess(info.Process, &code);
        CloseHandle(info.Thread);
        CloseHandle(info.Process);

        printf("  [parent] child exit code=%lu\n", code);
        printf("  [parent] counter=%ld  buffer=%s\n", g_counter, g_buffer);

        int ok = (g_counter == 1) && (strcmp(g_buffer, "BEFORE-CLONE") == 0);
        printf("\n%s: clone isolation %s (parent state %s)\n",
               ok ? "PASS" : "FAIL",
               ok ? "WORKS" : "BROKEN",
               ok ? "untouched by the clone" : "CORRUPTED by the clone");
        return ok ? 0 : 3;
    }

    printf("FAIL: RtlCloneUserProcess returned 0x%08lX\n", (unsigned long)st);
    return 4;
}
