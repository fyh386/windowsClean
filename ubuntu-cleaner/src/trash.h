#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

struct TrashStats {
    size_t ok = 0;
    size_t failed = 0;
    size_t permanent = 0; // deleted for real because trash was not supported
    std::vector<std::string> failedPaths;
};

// Moves files to the XDG trash (GIO). Files on volumes without a trash
// location are deleted permanently and counted in `permanent`. `progress` is
// called from the worker thread; returns stats.
TrashStats DeletePathsToTrash(const std::vector<std::string>& paths,
                              std::function<void(size_t done, size_t total)> progress);

// Permanently deletes files (unlink). Used when the user confirms after a
// trash failure.
TrashStats DeletePathsPermanently(const std::vector<std::string>& paths,
                                  std::function<void(size_t done, size_t total)> progress);
