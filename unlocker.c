#include <windows.h>
#include <tlhelp32.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define TARGET "Ferns.exe"

static const float  cap60f = 1.0f / 60.0f;
static const double cap60d = 1.0  / 60.0;
static const float  cap30f = 1.0f / 30.0f;
static const double cap30d = 1.0  / 30.0;
static const float  repf   = 1.0f / 9999.0f;
static const double repd   = 1.0  / 9999.0;

typedef struct { const void *needle; const void *rep; int len; } Cap;

static Cap caps[4];

static void init_caps(void)
{
    caps[0].needle = &cap60f; caps[0].rep = &repf; caps[0].len = 4;
    caps[1].needle = &cap60d; caps[1].rep = &repd; caps[1].len = 8;
    caps[2].needle = &cap30f; caps[2].rep = &repf; caps[2].len = 4;
    caps[3].needle = &cap30d; caps[3].rep = &repd; caps[3].len = 8;
}

static DWORD find_pid(void)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;

    PROCESSENTRY32 pe = { sizeof(pe) };
    DWORD pid = 0;

    if (Process32First(snap, &pe)) {
        do {
            if (_stricmp(pe.szExeFile, TARGET) == 0) {
                pid = pe.th32ProcessID;
                break;
            }
        } while (Process32Next(snap, &pe));
    }
    CloseHandle(snap);
    return pid;
}

static int patch(HANDLE h)
{
    MEMORY_BASIC_INFORMATION mbi;
    uintptr_t addr = 0;
    int total = 0;

    while (VirtualQueryEx(h, (LPCVOID)addr, &mbi, sizeof(mbi))) {
        if (mbi.RegionSize == 0) break;

        DWORD p = mbi.Protect;
        if (mbi.State == MEM_COMMIT && (
            p == PAGE_READWRITE         ||
            p == PAGE_WRITECOPY         ||
            p == PAGE_EXECUTE_READWRITE ||
            p == PAGE_EXECUTE_WRITECOPY)) {

            uint8_t *buf = (uint8_t *)malloc(mbi.RegionSize);
            if (buf) {
                SIZE_T rd = 0;
                if (ReadProcessMemory(h, (LPCVOID)addr, buf, mbi.RegionSize, &rd) && rd > 0) {
                    for (int c = 0; c < 4; c++) {
                        int n = caps[c].len;
                        for (SIZE_T i = 0; i + n <= rd; i++) {
                            if (memcmp(buf + i, caps[c].needle, n) == 0) {
                                SIZE_T wr = 0;
                                WriteProcessMemory(h, (LPVOID)(addr + i), caps[c].rep, n, &wr);
                                if (wr) total++;
                            }
                        }
                    }
                }
                free(buf);
            }
        }
        addr += mbi.RegionSize;
    }
    return total;
}

int main(void)
{
    init_caps();

    printf("\n  ferns fps unlocker  v1.0\n");
    printf("  ------------------------------\n\n");

    DWORD pid = find_pid();
    if (!pid) {
        printf("  error: %s not running\n         open the game first\n", TARGET);
        goto done;
    }

    printf("  pid      %lu\n", pid);

    HANDLE h = OpenProcess(
        PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_VM_OPERATION | PROCESS_QUERY_INFORMATION,
        FALSE, pid);

    if (!h) {
        DWORD e = GetLastError();
        if (e == 5) printf("  error: access denied -- run as administrator\n");
        else        printf("  error: OpenProcess failed (%lu)\n", e);
        goto done;
    }

    printf("  scanning memory...\n\n");
    int n = patch(h);
    CloseHandle(h);

    if (n) {
        printf("  patched  %d value(s)\n  fps cap removed\n", n);
    } else {
        printf("  nothing found -- try running as administrator\n");
    }

done:
    printf("\n  press enter to exit ");
    getchar();
    return 0;
}
