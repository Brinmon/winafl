// harness_crypt32.c
//
// Demonstrates the exact harness pattern used to fuzz a CLOSED-SOURCE DLL
// with WinAFL (TinyInst mode). The target here is CryptStringToBinaryA()
// from crypt32.dll — a real, closed-source Windows parser that decodes
// base64/hex input. This is the same pattern you apply to a QQ/WeChat
// parsing DLL, except there you derive the function offset via RE instead
// of GetProcAddress.
//
// Pattern:
//   1. LoadLibrary  the closed-source DLL        (done once, OUTSIDE target)
//   2. GetProcAddress / offset -> the parse fn   (done once, OUTSIDE target)
//   3. target function reads the input file and calls the parse fn
//      (WinAFL hooks THIS function for persistent looping)
//
// Build (x64, needs PDB for -target_method):
//   cl /O2 /MD /Zi harness_crypt32.c /Fe:harness.exe /link /DEBUG:FULL
//
// Fuzz:
//   afl-fuzz.exe -y -i in -o out -- -instrument_module crypt32.dll ^
//     -target_module harness.exe -target_method fuzz_one -nargs 1 ^
//     -iterations 10000 -persist -loop -- harness.exe @@
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

typedef BOOL (WINAPI *PFN_CryptStringToBinaryA)(
    LPCSTR pszString, DWORD cchString, DWORD dwFlags,
    BYTE *pbBinary, DWORD *pcbBinary, DWORD *pdwSkip, DWORD *pdwFlags);

static PFN_CryptStringToBinaryA pCryptStringToBinaryA = NULL;

// This is the persistent fuzz target. WinAFL hooks it (found via PDB symbol
// "fuzz_one") and loops it, rewriting the input file between iterations.
// It must: open the input file, parse it, close it, and return normally.
__declspec(noinline) void fuzz_one(char *filename)
{
    FILE *f = fopen(filename, "rb");
    if (!f) return;
    char buf[4096];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';

    // decode as base64 (CRYPT_STRING_BASE64 = 0x1)
    DWORD cb = 0;
    pCryptStringToBinaryA(buf, (DWORD)n, 0x1, NULL, &cb, NULL, NULL);
    if (cb && cb < 1 << 20) {
        BYTE *out = (BYTE *)malloc(cb);
        if (out) {
            pCryptStringToBinaryA(buf, (DWORD)n, 0x1, out, &cb, NULL, NULL);
            free(out);
        }
    }
}

int main(int argc, char **argv)
{
    // 1+2: load DLL + resolve target ONCE, before the fuzz loop
    HMODULE h = LoadLibraryA("crypt32.dll");
    if (!h) { fprintf(stderr, "LoadLibrary crypt32 failed\n"); return 1; }
    pCryptStringToBinaryA =
        (PFN_CryptStringToBinaryA)GetProcAddress(h, "CryptStringToBinaryA");
    if (!pCryptStringToBinaryA) { fprintf(stderr, "GetProcAddress failed\n"); return 1; }

    // standalone single run (for smoke testing); under WinAFL the target
    // function is hooked and looped, so this just provides a manual entry.
    if (argc > 1) {
        fuzz_one(argv[1]);
        return 0;
    }
    fprintf(stderr, "usage: %s <input-file>\n", argv[0]);
    return 2;
}
