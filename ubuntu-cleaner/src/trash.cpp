#include "trash.h"

#include <gio/gio.h>

#include <cstdio>
#include <mutex>

TrashStats DeletePathsToTrash(const std::vector<std::string>& paths,
                              std::function<void(size_t done, size_t total)> progress) {
    TrashStats stats;
    std::mutex mtx;
    for (size_t i = 0; i < paths.size(); ++i) {
        GFile* g = g_file_new_for_path(paths[i].c_str());
        GError* err = nullptr;
        gboolean ok = g_file_trash(g, nullptr, &err);
        if (ok) {
            ++stats.ok;
        } else {
            ++stats.failed;
            std::lock_guard<std::mutex> lock(mtx);
            stats.failedPaths.push_back(paths[i]);
        }
        if (err) g_error_free(err);
        g_object_unref(g);
        if (progress) progress(i + 1, paths.size());
    }
    return stats;
}

TrashStats DeletePathsPermanently(const std::vector<std::string>& paths,
                                  std::function<void(size_t done, size_t total)> progress) {
    TrashStats stats;
    std::mutex mtx;
    for (size_t i = 0; i < paths.size(); ++i) {
        if (std::remove(paths[i].c_str()) == 0) {
            ++stats.permanent;
        } else {
            ++stats.failed;
            std::lock_guard<std::mutex> lock(mtx);
            stats.failedPaths.push_back(paths[i]);
        }
        if (progress) progress(i + 1, paths.size());
    }
    return stats;
}
