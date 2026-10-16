#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct MemInfo {
    uint64_t total = 0;
    uint64_t available = 0;
    uint64_t commitLimit = 0;
    uint64_t committed = 0;
};

bool GetMemInfo(MemInfo& m);

struct ProcEntry {
    int pid = 0;
    std::string name;
    uint64_t rss = 0;
};

void EnumProcesses(std::vector<ProcEntry>& out);
bool KillProcess(int pid); // SIGTERM

// sync + drop caches. Runs "pkexec" which may show a polkit dialog; call this
// off the UI thread. Returns true on success.
bool RunDropCaches();
