/* tools/crref/crref.c — native Win32 reference harness for CheckRevision 1.14d.
 *
 * No network. Loads the real CheckRevision.dll (extracted from the signed
 * MPQ) and calls its export with the same seed as the runtime shim, on a
 * real Win32 stack (CryptoAPI, CryptStringToBinaryW, CryptBinaryToStringW,
 * VerQueryValueW, WinVerifyTrust / CryptQueryObject).
 *
 * SCOPE. The DLL hashes LE32(seed) || ":<host EXE version>:" || Authenticode-byte.
 * The "host EXE" is the current process — this binary, not Game.exe:
 *   - the four-part version is read from this harness's own VERSION
 *     resource; crref.rc bakes in 1.14.3.71, the exact VS_FIXEDFILEINFO
 *     value of Game.exe (cross-checked by tools/checkrevision_ref.py, which
 *     reads it straight from the file).
 *   - the Authenticode byte is always 0: this harness isn't signed by
 *     Blizzard, and fabricating a signature is out of the question. So this
 *     leg can never produce auth=1, and auth=1 must never be inferred from
 *     it — a comparator whose two legs can both only fail closed always
 *     reports "equal". It only arbitrates the auth=0 line, but that already
 *     exercises SHA-1, base64, the `info` format, and the VERSION resource
 *     read as independent checks.
 *
 * Build: i686-w64-mingw32-gcc -O2 crref.c crref.o.res -o Game.exe
 * (see tools/oracle_checkrevision.sh)
 */
#include <windows.h>
#include <stdio.h>

typedef BOOL (__stdcall *CR)(const char*, const char*, const char*,
                             const char*, DWORD*, DWORD*, char*);

int main(int argc, char** argv) {
    const char* dll  = argc > 1 ? argv[1] : "CheckRevision.dll";
    const char* seed = argc > 2 ? argv[2] : "dp26DAAA";
    wchar_t self[MAX_PATH]; GetModuleFileNameW(NULL, self, MAX_PATH);
    DWORD dummy = 0, vsz = GetFileVersionInfoSizeW(self, &dummy);
    printf("harness=%ls verinfo=%lu octets\n", self, (unsigned long)vsz);
    if (vsz) {
        void* blk = malloc(vsz);
        if (GetFileVersionInfoW(self, 0, vsz, blk)) {
            VS_FIXEDFILEINFO* fi = NULL; UINT n = 0;
            if (VerQueryValueW(blk, L"\\", (LPVOID*)&fi, &n) && fi)
                printf("harness version = %u.%u.%u.%u\n",
                       HIWORD(fi->dwFileVersionMS), LOWORD(fi->dwFileVersionMS),
                       HIWORD(fi->dwFileVersionLS), LOWORD(fi->dwFileVersionLS));
        }
        free(blk);
    }
    HMODULE h = LoadLibraryA(dll);
    if (!h) { printf("CRREF-RESULT ERROR LoadLibrary(%s) -> %lu\n", dll, GetLastError()); return 2; }
    CR fn = (CR)GetProcAddress(h, "CheckRevision");
    if (!fn) { printf("CRREF-RESULT ERROR GetProcAddress -> %lu\n", GetLastError()); return 2; }
    DWORD ver = 0xDEADBEEF, ck = 0xDEADBEEF;
    char info[256]; memset(info, 0xAA, sizeof info);
    BOOL r = fn("", "", "", seed, &ver, &ck, info);
    /* `info` is a string: print it as-is, escaping the newline — this is
       exactly the byte this harness exists to arbitrate. */
    char esc[512]; int k = 0;
    for (int i = 0; i < 250 && info[i] && k < 500; i++) {
        if (info[i] == '\n') { esc[k++] = '\\'; esc[k++] = 'n'; }
        else esc[k++] = info[i];
    }
    esc[k] = 0;
    printf("CRREF-RESULT seed=%s ret=%d version=0x%08lx checksum=0x%08lx info=%s\n",
           seed, (int)r, (unsigned long)ver, (unsigned long)ck, esc);
    return 0;
}
