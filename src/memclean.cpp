#include "memclean.h"

#include "util.h"

#define PSAPI_VERSION 2
#include <psapi.h>
#include <tlhelp32.h>

namespace {

constexpr int kSystemMemoryListInformation = 0x50;   // SystemMemoryListInformation
constexpr ULONG kMemoryPurgeStandbyList = 4;

typedef LONG(WINAPI* NtSetSystemInformationFn)(ULONG, PVOID, ULONG);

bool IsSameUser(DWORD pid) {
    HANDLE hp = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!hp) return false;
    HANDLE tok = nullptr;
    if (!OpenProcessToken(hp, TOKEN_QUERY, &tok)) { CloseHandle(hp); return false; }

    DWORD sz = 0;
    GetTokenInformation(tok, TokenUser, nullptr, 0, &sz);
    std::vector<unsigned char> buf(sz ? sz : 1);
    BOOL okTok = GetTokenInformation(tok, TokenUser, buf.data(), (DWORD)buf.size(), &sz);
    CloseHandle(tok);
    if (!okTok) { CloseHandle(hp); return false; }

    HANDLE self = GetCurrentProcess();
    HANDLE selfTok = nullptr;
    if (!OpenProcessToken(self, TOKEN_QUERY, &selfTok)) { CloseHandle(hp); return false; }
    DWORD sz2 = 0;
    GetTokenInformation(selfTok, TokenUser, nullptr, 0, &sz2);
    std::vector<unsigned char> buf2(sz2 ? sz2 : 1);
    BOOL okSelf = GetTokenInformation(selfTok, TokenUser, buf2.data(), (DWORD)buf2.size(), &sz2);
    CloseHandle(selfTok);
    CloseHandle(hp);
    if (!okSelf) return false;

    TOKEN_USER* tu = (TOKEN_USER*)buf.data();
    TOKEN_USER* tu2 = (TOKEN_USER*)buf2.data();
    return EqualSid(tu->User.Sid, tu2->User.Sid) == TRUE;
}

} // namespace

bool GetMemInfo(MemInfo& m) {
    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof(ms);
    if (!GlobalMemoryStatusEx(&ms)) return false;
    m.totalPhys = ms.ullTotalPhys;
    m.availPhys = ms.ullAvailPhys;
    m.totalCommit = ms.ullTotalPageFile;
    m.availCommit = ms.ullAvailPageFile;
    return true;
}

bool PurgeStandbyList() {
    HMODULE ntdll = LoadLibraryW(L"ntdll.dll");
    if (!ntdll) return false;
    auto fn = (NtSetSystemInformationFn)GetProcAddress(ntdll, "NtSetSystemInformation");
    if (!fn) { FreeLibrary(ntdll); return false; }
    ULONG action = kMemoryPurgeStandbyList;
    LONG st = fn(kSystemMemoryListInformation, &action, sizeof(action));
    FreeLibrary(ntdll);
    return st >= 0;
}

size_t TrimWorkingSets(bool userOnly) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;

    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    size_t trimmed = 0;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (pe.th32ProcessID == GetCurrentProcessId()) continue;
            if (userOnly && !IsSameUser(pe.th32ProcessID)) continue;
            HANDLE h = OpenProcess(PROCESS_SET_QUOTA | PROCESS_QUERY_LIMITED_INFORMATION,
                                   FALSE, pe.th32ProcessID);
            if (!h) continue;
            if (SetProcessWorkingSetSize(h, (SIZE_T)-1, (SIZE_T)-1)) ++trimmed;
            CloseHandle(h);
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return trimmed;
}

void EnumProcessEntries(std::vector<ProcEntry>& out) {
    out.clear();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return;

    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            ProcEntry e;
            e.pid = pe.th32ProcessID;
            e.name = pe.szExeFile;

            HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, e.pid);
            if (h) {
                PROCESS_MEMORY_COUNTERS_EX pmc{};
                pmc.cb = sizeof(pmc);
                if (GetProcessMemoryInfo(h, (PPROCESS_MEMORY_COUNTERS)&pmc, sizeof(pmc))) {
                    e.workingSet = pmc.WorkingSetSize;
                    e.privateBytes = pmc.PrivateUsage;
                }
                CloseHandle(h);
            }
            out.push_back(std::move(e));
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
}

bool KillProcess(DWORD pid) {
    HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
    if (!h) return false;
    BOOL ok = TerminateProcess(h, 1);
    CloseHandle(h);
    return ok != FALSE;
}
