#include "scanner.h"

#include "probe.h"
#include "util.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <mutex>
#include <set>
#include <thread>
#include <utility>

namespace fs = std::filesystem;

namespace {

const char* kHomeEnv() {
    const char* h = getenv("HOME");
    return h ? h : "/root";
}

bool IsSystemExcluded(const std::string& path) {
    // Never list files already inside a trash directory, nor virtual FSes.
    static const std::vector<std::string> kSegments = {
        "/.local/share/Trash/", "/proc/", "/sys/", "/dev/", "/run/",
    };
    std::string lower = LowerAscii(path);
    for (const auto& seg : kSegments) {
        if (lower.find(seg) != std::string::npos) return true;
    }
    return false;
}

bool ShouldSkipDir(const std::string& path) {
    static const std::vector<std::string> kSkip = {
        "/proc", "/sys", "/dev", "/run", "/snap", "/lost+found",
    };
    for (const auto& s : kSkip) {
        if (path == s) return true;
    }
    return false;
}

bool PathExcluded(const std::string& path, const std::vector<std::string>& excludes) {
    for (const auto& e : excludes) {
        if (StartsWithIgnoreCase(path, e)) return true;
    }
    return false;
}

time_t ToTimeT(const fs::file_time_type& ft) {
    auto secs = std::chrono::duration_cast<std::chrono::seconds>(
                    ft.time_since_epoch())
                    .count();
    return (time_t)secs;
}

bool IsExecutableFile(const fs::file_status& st) {
    auto p = st.permissions();
    using perm = fs::perms;
    return (p & (perm::owner_exec | perm::group_exec | perm::others_exec)) != perm::none;
}

bool IsProgramFile(const std::string& path) {
    static const std::set<std::string> kProgExts = {
        "exe", "com", "scr", "bat", "cmd", "msi", "msp", "lnk", "ps1",
        "vbs", "jar", "appref-ms", "AppImage", "run", "sh", "desktop", "bin",
    };
    std::string ext = ExtOf(path);
    return kProgExts.count(ext) != 0;
}

// Applies the page-specific filter. Returns true if the file should be kept.
bool PassesFilter(ScanPage page, const FileItem& fi, const ScanOptions& opts,
                  bool* wantsProbe) {
    *wantsProbe = false;
    std::string ext = ExtOf(fi.path);
    switch (page) {
    case ScanPage::Log: {
        ExtClass cls = ClassifyExt(ext);
        if (cls == ExtClass::Log) return true;
        if (cls == ExtClass::NotLog) return false;
        *wantsProbe = opts.probe;
        return false;
    }
    case ScanPage::Large:
        return fi.size >= opts.minSizeBytes;
    case ScanPage::Archive:
        return IsArchiveExt(ext);
    case ScanPage::Installer:
        return IsInstallerExt(ext);
    case ScanPage::Idle: {
        if (opts.idleDays <= 0) return false;
        time_t cutoff = time(nullptr) - (time_t)opts.idleDays * 86400;
        if (fi.modified > cutoff) return false;
        if (opts.idleMode == 1) {
            // Executable bit OR known program extension.
            std::error_code ec;
            auto st = fs::symlink_status(fi.path, ec);
            if (ec) return false;
            if (!IsExecutableFile(st) && !IsProgramFile(fi.path)) return false;
        }
        return true;
    }
    }
    return false;
}

struct WalkShared {
    std::mutex mtx;
    std::condition_variable cv;
    std::deque<std::string> dirs;
    bool done = false;
};

void WalkWorker(ScanPage page, const ScanOptions& opts, WalkShared& shared,
                std::vector<FileItem>& out, std::atomic<size_t>& scanned,
                std::atomic<size_t>& probeCandidates, std::atomic<bool>& tooMany) {
    for (;;) {
        std::string dir;
        {
            std::unique_lock<std::mutex> lock(shared.mtx);
            shared.cv.wait(lock, [&] { return shared.done || !shared.dirs.empty(); });
            if (shared.dirs.empty()) return;
            dir = shared.dirs.front();
            shared.dirs.pop_front();
        }

        std::error_code ec;
        fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec);
        fs::directory_iterator end;
        if (ec) continue;
        for (; it != end; it.increment(ec)) {
            if (ec) { ec.clear(); continue; }
            const fs::directory_entry& entry = *it;
            std::error_code ec2;
            fs::file_status st = entry.symlink_status(ec2);
            if (ec2) continue;

            if (fs::is_directory(st)) {
                std::string sub = entry.path().string();
                if (ShouldSkipDir(sub) || IsSystemExcluded(sub)) continue;
                {
                    std::lock_guard<std::mutex> lock(shared.mtx);
                    shared.dirs.push_back(std::move(sub));
                }
                shared.cv.notify_one();
                continue;
            }
            if (!fs::is_regular_file(st)) continue;

            std::string path = entry.path().string();
            if (IsSystemExcluded(path)) continue;
            if (PathExcluded(path, opts.excludePaths)) continue;
            if (opts.scope == 1) {
                std::string homePrefix = std::string(kHomeEnv()) + "/";
                if (!StartsWithIgnoreCase(path, homePrefix)) continue;
            }

            FileItem fi;
            fi.path = std::move(path);
            fi.size = (uint64_t)entry.file_size(ec2);
            if (ec2) continue;
            fi.modified = ToTimeT(entry.last_write_time(ec2));
            if (ec2) continue;
            fi.kind = FileKind::Large;

            if (page == ScanPage::Log && opts.timeDays > 0) {
                time_t now = time(nullptr);
                if (now - fi.modified > (time_t)opts.timeDays * 86400) continue;
            }

            bool wantsProbe = false;
            if (!PassesFilter(page, fi, opts, &wantsProbe)) {
                if (wantsProbe) {
                    if (probeCandidates.fetch_add(1) < opts.probeCap) {
                        std::lock_guard<std::mutex> lock(shared.mtx);
                        out.push_back(std::move(fi));
                    }
                }
                continue;
            }

            if (page == ScanPage::Log) fi.kind = FileKind::KnownLog;
            else if (page == ScanPage::Idle) fi.kind = FileKind::Idle;
            else if (page == ScanPage::Archive) fi.kind = FileKind::Archive;
            else if (page == ScanPage::Installer) fi.kind = FileKind::Installer;
            else fi.kind = FileKind::Large;

            size_t n = scanned.fetch_add(1);
            if (n >= opts.resultCap) {
                tooMany.store(true);
                continue;
            }
            std::lock_guard<std::mutex> lock(shared.mtx);
            out.push_back(std::move(fi));
        }
    }
}

void ProbeCandidates(std::vector<FileItem>& candidates, const ScanOptions& opts,
                     size_t& probed, size_t& unreadable) {
    size_t n = std::min(candidates.size(), opts.probeCap);
    std::vector<unsigned char> keep(n, 0);
    std::atomic<size_t> next{ 0 };
    std::atomic<size_t> unread{ 0 };
    unsigned nThreads = std::max(1u, std::min(8u, std::thread::hardware_concurrency()));
    std::vector<std::thread> pool;
    for (unsigned t = 0; t < nThreads; ++t) {
        pool.emplace_back([&] {
            for (;;) {
                size_t idx = next.fetch_add(1);
                if (idx >= n) break;
                ProbeOut po = ProbeFile(candidates[idx].path, candidates[idx].size,
                                        opts.probeMaxBytes);
                if (po.kind == ProbeKind::Text &&
                    (!opts.onlyMultiline || po.multiline)) {
                    keep[idx] = 1;
                } else if (po.kind == ProbeKind::Unreadable) {
                    unread.fetch_add(1);
                }
            }
        });
    }
    for (auto& t : pool) t.join();

    FileVec kept;
    kept.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        if (keep[i]) {
            candidates[i].kind = FileKind::ProbedText;
            kept.push_back(std::move(candidates[i]));
        }
    }
    candidates = std::move(kept);
    probed = n;
    unreadable = unread.load();
}

} // namespace

void StartScan(ScanPage page, const ScanOptions& opts,
               std::function<void(ScanResult)> done) {
    std::thread([page, opts, done = std::move(done)] {
        uint64_t t0 = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now().time_since_epoch())
                          .count();
        ScanResult res;
        res.page = page;

        WalkShared shared;
        if (opts.scope == 1) {
            shared.dirs.push_back(kHomeEnv());
        } else {
            std::error_code ec;
            fs::directory_iterator it("/", fs::directory_options::skip_permission_denied, ec);
            fs::directory_iterator end;
            if (!ec) {
                for (; it != end; it.increment(ec)) {
                    if (ec) { ec.clear(); continue; }
                    const fs::directory_entry& e = *it;
                    std::error_code ec2;
                    if (fs::is_directory(e.symlink_status(ec2)) && !ec2) {
                        std::string p = e.path().string();
                        if (!ShouldSkipDir(p)) shared.dirs.push_back(std::move(p));
                    }
                }
            }
        }
        if (shared.dirs.empty()) {
            res.ok = false;
            res.error = "没有可扫描的目录（权限不足？）";
            done(std::move(res));
            return;
        }

        FileVec base;
        std::atomic<size_t> scanned{ 0 };
        std::atomic<size_t> probeCandidates{ 0 };
        std::atomic<bool> tooMany{ false };

        unsigned nThreads = std::max(1u, std::min(8u, std::thread::hardware_concurrency()));
        std::vector<std::thread> pool;
        for (unsigned t = 0; t < nThreads; ++t) {
            pool.emplace_back([&] { WalkWorker(page, opts, shared, base, scanned,
                                               probeCandidates, tooMany); });
        }
        {
            std::lock_guard<std::mutex> lock(shared.mtx);
            shared.done = true;
        }
        shared.cv.notify_all();
        for (auto& t : pool) t.join();

        res.truncated = tooMany.load() || base.size() >= opts.resultCap;
        if (base.size() > opts.resultCap) base.resize(opts.resultCap);

        if (page == ScanPage::Log && opts.probe && !base.empty()) {
            // Separate extensionless/unknown candidates (already collected via
            // wantsProbe into `base`), then probe them and keep text files.
            // Keep only probed items: everything in base for a log scan that is
            // not a known log is a probe candidate.
            FileVec candidates;
            for (auto& fi : base) {
                if (fi.kind != FileKind::KnownLog) candidates.push_back(std::move(fi));
            }
            base.erase(std::remove_if(base.begin(), base.end(),
                                      [](const FileItem& f) {
                                          return f.kind != FileKind::KnownLog;
                                      }),
                       base.end());
            size_t probed = 0, unreadable = 0;
            ProbeCandidates(candidates, opts, probed, unreadable);
            res.probed = probed;
            res.unreadable = unreadable;
            if (base.size() + candidates.size() > opts.resultCap) {
                res.truncated = true;
                candidates.resize(opts.resultCap - std::min(base.size(), opts.resultCap));
            }
            for (auto& fi : candidates) base.push_back(std::move(fi));
        } else {
            for (auto& fi : base) {
                if (page == ScanPage::Large) fi.kind = FileKind::Large;
                else if (page == ScanPage::Archive) fi.kind = FileKind::Archive;
                else if (page == ScanPage::Installer) fi.kind = FileKind::Installer;
                else if (page == ScanPage::Idle) fi.kind = FileKind::Idle;
            }
        }

        res.items = std::move(base);
        res.elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count() -
                        t0;
        done(std::move(res));
    }).detach();
}
