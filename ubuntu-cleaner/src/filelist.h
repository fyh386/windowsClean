#pragma once

#include <cstdint>
#include <ctime>
#include <string>
#include <vector>

enum class FileKind : uint8_t {
    KnownLog = 0,
    ProbedText = 1,
    Large = 2,
    Idle = 3,
    Archive = 4,
    Installer = 5,
};

struct FileItem {
    std::string path;
    uint64_t size = 0;
    time_t modified = 0;
    time_t accessed = 0;
    FileKind kind = FileKind::Large;
};

using FileVec = std::vector<FileItem>;

// Compares by column: 0/1 name, 2 path, 3 size, 4 time. Ties break by path.
int CompareFileItems(const FileItem& a, const FileItem& b, int col, bool asc);
// Builds a permutation of [0, n) sorted by col.
void BuildSortedOrder(const FileVec& items, std::vector<unsigned>& order, int col, bool asc);
