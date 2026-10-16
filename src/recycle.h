#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

struct DeleteStats {
    size_t ok = 0;
    size_t failed = 0;
    size_t permanent = 0; // items deleted permanently (remote volumes)
    std::vector<std::wstring> submitted; // paths handed to the delete call
    std::vector<std::wstring> failedPaths;
};

using DeleteProgress = std::function<void(size_t done, size_t total)>;

// Moves paths to the recycle bin when possible. Files on remote volumes are
// permanently deleted (the caller confirms this with the user beforehand).
DeleteStats DeletePathsToRecycleBin(const std::vector<std::wstring>& paths,
                                    DeleteProgress progress = nullptr);

bool WritePathListFile(const std::wstring& file, const std::vector<std::wstring>& paths);
bool ReadPathListFile(const std::wstring& file, std::vector<std::wstring>& out);
