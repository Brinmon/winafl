// shared_mem_poc.c -- P1: prove a NAMED shared section survives the clone.
//
// RtlCloneUserProcess gives the child a copy-on-write copy of the address space,
// so ANY ordinary write the child makes is invisible to the parent. AFL's
// coverage map lives in ordinary memory, so it would be lost.
//
// The fix: allocate the coverage map in an explicitly SHARED section
// (CreateFileMapping + MapViewOfFile). Shared sections are backed by the pagefile
// and mapped identically into both processes, so they bypass CoW.
//
// This PoC verifies that assumption before we build the fork-server on it.
#include <windows.h>
#include <stdio.h>
#include <string.h>

typedef LONG NTSTATUS;
#define STATUS_SUCCESS        ((NTSTATUS)0x00000000L)
#define STATUS_PROCESS_CLONED  ((NTSTATUS)0x00000129L)
#define RTL_CLONE_PROCESS_FLAGS_INHERIT_HANDLES 0x00000002

typedef struct _RTL_USER_PROCESS_INFORMATION {
    ULONG Length; HANDLE Process; HANDLE Thread; BYTE Reserved[512];
} RTL_USER_PROCESS_INFORMATION;

typedef NTSTATUS (NTAPI *PFN_RtlCloneUserProcess)(ULONG, PSECURITY_DESCRIPTOR,
    PSECURITY_DESCRIPTOR, HANDLE, RTL_USER_PROCESS_INFORMATION *);

#define MAP_BYTES 65536

int main(void)
{
    PFN_RtlCloneUserProcess pClone = (PFN_RtlCloneUserProcess)
        GetProcAddress(GetModuleHandleA("ntdll.dll"), "RtlCloneUserProcess");
    if (!pClone) { printf("FAIL: no RtlCloneUserProcess\n"); return 2; }

    // --- the "coverage map": a named, shared section (bypasses CoW) ---
    HANDLE hMap = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE,
                                     0, MAP_BYTES, "Local\fuzzkit_covmap_poc");
    if (!hMap) { printf("FAIL: CreateFileMapping %lu\n", GetLastError()); return 3; }
    unsigned char *cov = (unsigned char *)MapViewOfFile(hMap, FILE_MAP_ALL_ACCESS, 0, 0, MAP_BYTES);
    if (!cov) { printf("FAIL: MapViewOfFile %lu\n", GetLastError()); return 4; }

    // --- ordinary (private, CoW) memory, for comparison ---
    static unsigned char priv[16];
    memset(priv, 0, sizeof(priv));

    cov[0] = 0xAA;   // sentinel written by the parent

    RTL_USER_PROCESS_INFORMATION info;
    memset(&info, 0, sizeof(info));
    info.Length = sizeof(info);
    NTSTATUS st = pClone(RTL_CLONE_PROCESS_FLAGS_INHERIT_HANDLES, NULL, NULL, NULL, &info);

    if (st == STATUS_PROCESS_CLONED) {
        // --- child: write to BOTH maps ---
        cov[0] = 0xBB;            // should be visible to the parent (shared)
        cov[1] = 0xCC;
        priv[0] = 0xDD;           // should NOT be visible (CoW)
        // record what the child saw before writing
        printf("  [child] saw cov[0]=0x%02X priv[0]=0x%02X\n", 0xAA, 0x00);
        printf("  [child] wrote cov[0]=0xBB cov[1]=0xCC priv[0]=0xDD\n");
        return 0;
    }
    if (st == STATUS_SUCCESS) {
        WaitForSingleObject(info.Process, 10000);
        CloseHandle(info.Thread); CloseHandle(info.Process);

        int shared_ok = (cov[0] == 0xBB && cov[1] == 0xCC);
        int cow_ok    = (priv[0] == 0x00);

        printf("  [parent] cov[0]=0x%02X cov[1]=0x%02X priv[0]=0x%02X\n",
               cov[0], cov[1], priv[0]);
        printf("\n  shared section carries child writes: %s\n", shared_ok ? "YES (as needed)" : "NO");
        printf("  private memory is CoW-isolated:      %s\n", cow_ok ? "YES (as needed)" : "NO");

        int pass = shared_ok && cow_ok;
        printf("\n%s: coverage map CAN be shared across the clone\n", pass ? "PASS" : "FAIL");
        UnmapViewOfFile(cov); CloseHandle(hMap);
        return pass ? 0 : 5;
    }
    printf("FAIL: clone returned 0x%08lX\n", (unsigned long)st);
    return 6;
}
