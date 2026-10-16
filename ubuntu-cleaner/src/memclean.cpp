#include "memclean.h"

#include "util.h"

#include <dirent.h>
#include <signal.h>
#include <sys/stat.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>

namespace fs = std::filesystem;

namespace {

uint64_t ParseMemValue(const std::string& v) {
    size_t sp = v.find(' ');
    std::string num = v.substr(0, sp);
    return strtoull(num.c_str(), nullptr, 10) * 1024;
}

} // namespace

bool GetMemInfo(MemInfo& m) {
    std::string content;
    if (!ReadSmallFile("/proc/meminfo", content)) return false;
    for (const auto& line : SplitLines(content)) {
        if (line.rfind("MemTotal:", 0) == 0) m.total = ParseMemValue(Trim(line.substr(9)));
        else if (line.rfind("MemAvailable:", 0) == 0) m.available = ParseMemValue(Trim(line.substr(13)));
        else if (line.rfind("CommitLimit:", 0) == 0) m.commitLimit = ParseMemValue(Trim(line.substr(12)));
        else if (line.rfind("Committed_AS:", 0) == 0) m.committed = ParseMemValue(Trim(line.substr(13)));
    }
    return m.total != 0;
}

void EnumProcesses(std::vector<ProcEntry>& out) {
    out.clear();
    std::error_code ec;
    fs::directory_iterator it("/proc", fs::directory_options::skip_permission_denied, ec);
    fs::directory_iterator end;
    if (ec) return;
    for (; it != end; it.increment(ec)) {
        if (ec) { ec.clear(); continue; }
        std::string name = it->path().filename().string();
        bool numeric = !name.empty();
        for (char c : name) {
            if (c < '0' || c > '9') { numeric = false; break; }
        }
        if (!numeric) continue;
        int pid = atoi(name.c_str());
        std::string base = "/proc/" + name + "/";

        std::string comm;
        ReadSmallFile(base + "comm", comm);
        comm = Trim(comm);

        uint64_t rss = 0;
        std::string status;
        if (ReadSmallFile(base + "status", status)) {
            for (const auto& line : SplitLines(status)) {
                if (line.rfind("VmRSS:", 0) == 0) {
                    rss = ParseMemValue(Trim(line.substr(6)));
                    break;
                }
            }
        }
        ProcEntry e;
        e.pid = pid;
        e.name = comm.empty() ? name : comm;
        e.rss = rss;
        out.push_back(std::move(e));
    }
}

bool KillProcess(int pid) {
    return ::kill(pid, SIGTERM) == 0;
}

bool RunDropCaches() {
    const char* cmd =
        "pkexec /bin/sh -c 'sync; echo 3 > /proc/sys/vm/drop_caches' 2>/dev/null";
    FILE* p = popen(cmd, "r");
    if (!p) return false;
    int rc = pclose(p);
    return rc == 0;
}
