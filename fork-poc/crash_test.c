// crash_test.c -- the last unknown: if the CLONE crashes, does the parent survive
// and can it keep fuzzing? Plus a throughput measurement of clone+parse.
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef LONG NTSTATUS;
#define STATUS_SUCCESS        ((NTSTATUS)0x00000000L)
#define STATUS_PROCESS_CLONED  ((NTSTATUS)0x00000129L)
#define RTL_CLONE_PROCESS_FLAGS_INHERIT_HANDLES 0x00000002
typedef struct _RTL_USER_PROCESS_INFORMATION { ULONG Length; HANDLE P; HANDLE T; BYTE R[512]; } RTL_USER_PROCESS_INFORMATION;
typedef NTSTATUS (NTAPI *PFN_clone)(ULONG, PSECURITY_DESCRIPTOR, PSECURITY_DESCRIPTOR, HANDLE, RTL_USER_PROCESS_INFORMATION *);
typedef void *(*PFN_parse)(const void *, size_t, const char *, ...);
typedef void  (*PFN_init)(const char *);

static PFN_clone p_clone; static PFN_parse p_parse;
static unsigned char *g_in; static size_t g_in_len;
static volatile LONG *g_ctl;   // [0]=parse_ok [1]=crash_mode

int main(int argc, char **argv)
{
    int n = (argc > 1) ? atoi(argv[1]) : 500;
    int crash_mode = (argc > 2) ? atoi(argv[2]) : 0;   // 1 = clone dereferences NULL
    SetCurrentDirectoryA(".");
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    p_clone = (PFN_clone)GetProcAddress(GetModuleHandleA("ntdll.dll"), "RtlCloneUserProcess");
    HMODULE hv = LoadLibraryA("libvips-42.dll");
    if (!hv) { printf("FATAL libvips\n"); return 3; }
    p_parse = (PFN_parse)GetProcAddress(hv, "vips_image_new_from_buffer");
    PFN_init vi = (PFN_init)GetProcAddress(hv, "vips_init"); if (vi) vi("crash_test");

    FILE *f = fopen("seed.png","rb"); static unsigned char buf[65536];
    if (f) { g_in_len = fread(buf,1,sizeof(buf),f); fclose(f); } g_in = buf;
    if (p_parse) p_parse(g_in, g_in_len, "", NULL);   // warmup

    HANDLE hm = CreateFileMappingA(INVALID_HANDLE_VALUE,NULL,PAGE_READWRITE,0,4096,"Local\crashctl");
    g_ctl = (volatile LONG *)MapViewOfFile(hm, FILE_MAP_ALL_ACCESS, 0,0,4096);

    int ok=0, crashed=0, other=0;
    DWORD t0 = GetTickCount();
    for (int i = 0; i < n; i++) {
        g_ctl[0] = 0;
        RTL_USER_PROCESS_INFORMATION info; memset(&info,0,sizeof(info)); info.Length=sizeof(info);
        NTSTATUS st = p_clone(RTL_CLONE_PROCESS_FLAGS_INHERIT_HANDLES,NULL,NULL,NULL,&info);
        if (st == STATUS_PROCESS_CLONED) {
            if (crash_mode) {
                volatile int *p = NULL; *p = 1;      // deliberate access violation
            } else {
                void *img = p_parse ? p_parse(g_in, g_in_len, "", NULL) : NULL;
                g_ctl[0] = img ? 1 : 0;
            }
            ExitProcess(0);
        }
        if (st == STATUS_SUCCESS) {
            WaitForSingleObject(info.P, 15000);
            DWORD code = 0; GetExitCodeProcess(info.P, &code);
            CloseHandle(info.T); CloseHandle(info.P);
            if (code == 0) ok++;
            else if (code == 0xC0000005 || code == 0xC0000409 || code == 0xC000001D) crashed++;
            else other++;
        }
    }
    DWORD dt = GetTickCount() - t0;
    printf("clones=%d crash_mode=%d\n", n, crash_mode);
    printf("  exit 0 (clean)      : %d\n", ok);
    printf("  exit !=0 (crash)    : %d   <- parent SURVIVED and detected them\n", crashed);
    printf("  other               : %d\n", other);
    printf("  parent alive        : YES (we are printing this)\n");
    printf("  elapsed             : %lums  (%.0f clones/sec)\n", dt, n*1000.0/(dt?dt:1));
    return 0;
}
