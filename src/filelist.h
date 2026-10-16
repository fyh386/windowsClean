#pragma once

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

struct FileItem {
    std::wstring path;
    uint64_t size = 0;
    FILETIME modified{};
    FILETIME created{};
    FILETIME accessed{};
    FILETIME dateRun{};      // last run (Everything run history)
    uint32_t runCount = 0;   // run count (Everything run history)
    DWORD attrib = 0;
    uint8_t kind = 0; // 0 = known log, 1 = probed text, 2 = large file, 3 = idle
};

using FileVec = std::vector<FileItem>;

// idleCols maps column 4 -> last-run date and column 5 -> run count (the
// "久未使用" page); otherwise column 4 is modified and column 5 is kind.
int CompareFileItems(const FileItem& a, const FileItem& b, int col, bool asc, bool idleCols = false);
// Rebuilds order as a permutation of indices [0, items.size()) sorted by col.
void BuildSortedOrder(const FileVec& items, std::vector<unsigned>& order, int col, bool asc,
                      bool idleCols = false);
