#pragma once

#include "filelist.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

enum class ScanPage { Log = 0, Large = 1, Idle = 2, Archive = 3, Installer = 4 };

struct ScanOptions {
    int scope = 0;              // 0 = whole disk, 1 = home dir
    int timeDays = 0;           // log page: only files modified within N days (0 = all)
    bool probe = true;          // log page: probe extensionless / unknown ext
    bool onlyMultiline = true;  // log page: only keep probed text with newlines
    uint64_t minSizeBytes = 100ULL * 1024ULL * 1024ULL;
    int idleDays = 90;
    int idleMode = 0;           // 0 = all files, 1 = programs/executables only
    size_t resultCap = 200000;
    size_t probeCap = 50000;
    uint64_t probeMaxBytes = 64ULL * 1024ULL * 1024ULL;
    std::vector<std::string> excludePaths;
};

struct ScanResult {
    ScanPage page = ScanPage::Log;
    bool ok = true;
    std::string error;
    FileVec items;
    size_t probed = 0;
    size_t unreadable = 0;
    bool truncated = false;
    uint64_t elapsedMs = 0;
};

// Runs the scan on background threads and delivers the result through `done`
// (called on a worker thread; the UI must marshal it back to the main loop).
void StartScan(ScanPage page, const ScanOptions& opts,
               std::function<void(ScanResult)> done);
