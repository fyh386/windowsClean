#pragma once

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

struct MemInfo {
    uint64_t totalPhys = 0;
    uint64_t availPhys = 0;
    uint64_t totalCommit = 0;
    uint64_t availCommit = 0;
};

bool GetMemInfo(MemInfo& m);
bool PurgeStandbyList(); // requires administrator
size_t TrimWorkingSets(bool userOnly);

struct ProcEntry {
    DWORD pid = 0;
    std::wstring name;
    uint64_t workingSet = 0;
    uint64_t privateBytes = 0;
    // Pre-formatted display strings. The list view retains these text
    // pointers, so they must outlive the rows (no temporary sources).
    std::wstring pidText;
    std::wstring wsText;
    std::wstring privText;
};

void EnumProcessEntries(std::vector<ProcEntry>& out);
bool KillProcess(DWORD pid);
