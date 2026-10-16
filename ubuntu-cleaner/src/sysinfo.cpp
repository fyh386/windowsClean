#include "sysinfo.h"

#include "util.h"

#include <sys/statvfs.h>
#include <sys/utsname.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <set>

namespace fs = std::filesystem;

namespace {

uint64_t ParseMemValue(const std::string& v) {
    // "/proc/meminfo" values look like "16384000 kB".
    size_t sp = v.find(' ');
    std::string num = v.substr(0, sp);
    return strtoull(num.c_str(), nullptr, 10) * 1024;
}

std::string ReadTrimmed(const std::string& path) {
    std::string s;
    if (!ReadSmallFile(path, s)) return "";
    return Trim(s);
}

std::string CollectOs() {
    std::string content;
    if (ReadSmallFile("/etc/os-release", content)) {
        for (const auto& line : SplitLines(content)) {
            if (line.rfind("PRETTY_NAME=", 0) == 0) {
                std::string v = line.substr(12);
                if (v.size() >= 2 && v.front() == '"' && v.back() == '"') {
                    v = v.substr(1, v.size() - 2);
                }
                return v;
            }
        }
    }
    return "Linux";
}

std::string CollectKernel() {
    struct utsname u{};
    if (uname(&u) != 0) return "";
    return std::string(u.sysname) + " " + u.release;
}

std::string CollectHostname() {
    char buf[256] = {};
    if (gethostname(buf, sizeof(buf) - 1) != 0) return "";
    return buf;
}

std::string CollectCpu() {
    std::string content;
    if (!ReadSmallFile("/proc/cpuinfo", content)) return "未知";
    std::string model;
    int logical = 0;
    std::set<std::string> cores;
    std::string curCore, curPhys;
    for (const auto& line : SplitLines(content)) {
        size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string key = Trim(line.substr(0, colon));
        std::string val = Trim(line.substr(colon + 1));
        if (key == "model name" && model.empty()) model = val;
        else if (key == "processor") ++logical;
        else if (key == "core id") curCore = val;
        else if (key == "physical id") curPhys = val;
        if (key == "processor") {
            if (!curCore.empty() || !curPhys.empty()) {
                cores.insert(curPhys + ":" + curCore);
            }
            curCore.clear();
            curPhys.clear();
        }
    }
    if (model.empty()) model = "未知";
    char buf[512];
    snprintf(buf, sizeof(buf), "%s（物理 %zu 核 / 逻辑 %d 线程）",
             model.c_str(), cores.empty() ? (size_t)logical : cores.size(), logical);
    return buf;
}

std::string CollectGpu() {
    FILE* p = popen("lspci 2>/dev/null", "r");
    if (!p) return "未知";
    char line[512];
    std::string out;
    while (fgets(line, sizeof(line), p)) {
        std::string s = line;
        if (s.find("VGA") != std::string::npos ||
            s.find("3D controller") != std::string::npos ||
            s.find("Display controller") != std::string::npos) {
            size_t colon = s.find(':');
            size_t bracket = s.find(" [");
            std::string name;
            if (colon != std::string::npos) {
                name = Trim(s.substr(colon + 1));
            }
            if (!name.empty()) {
                // strip trailing vendor/device ids like [10de:1c03]
                size_t lb = name.find(" [");
                if (lb != std::string::npos) name = Trim(name.substr(0, lb));
                if (!out.empty()) out += " + ";
                out += name;
            }
        }
    }
    pclose(p);
    return out.empty() ? "未知" : out;
}

std::string CollectDmi(const char* file) {
    return ReadTrimmed(std::string("/sys/class/dmi/id/") + file);
}

std::string CollectDrives() {
    std::string mounts;
    if (!ReadSmallFile("/proc/mounts", mounts)) return "无";
    std::map<std::string, std::string> devToMount; // device -> mount point
    std::map<std::string, std::string> devToFstype;
    for (const auto& line : SplitLines(mounts)) {
        // dev mount fstype opts ...
        std::vector<std::string> parts;
        size_t start = 0;
        while (start <= line.size()) {
            size_t sp = line.find(' ', start);
            parts.push_back(line.substr(start, sp == std::string::npos ? std::string::npos : sp - start));
            if (sp == std::string::npos) break;
            start = sp + 1;
            while (start < line.size() && line[start] == ' ') ++start;
        }
        if (parts.size() < 3) continue;
        const std::string& dev = parts[0];
        const std::string& mp = parts[1];
        const std::string& fstype = parts[2];
        if (fstype == "proc" || fstype == "sysfs" || fstype == "devtmpfs" ||
            fstype == "devpts" || fstype == "tmpfs" || fstype == "cgroup" ||
            fstype == "cgroup2" || fstype == "pstore" || fstype == "securityfs" ||
            fstype == "debugfs" || fstype == "tracefs" || fstype == "configfs" ||
            fstype == "fusectl" || fstype == "mqueue" || fstype == "hugetlbfs" ||
            fstype == "binfmt_misc" || fstype == "overlay" || fstype == "squashfs" ||
            fstype == "autofs" || fstype == "bpf" || fstype == "rpc_pipefs") {
            continue;
        }
        if (dev.rfind("/dev/", 0) != 0) continue; // only real block devices
        // Prefer the shallowest mount point for a device.
        auto it = devToMount.find(dev);
        if (it == devToMount.end() || mp.size() < it->second.size()) {
            devToMount[dev] = mp;
            devToFstype[dev] = fstype;
        }
    }

    std::string out;
    for (const auto& [dev, mp] : devToMount) {
        struct statvfs sv{};
        if (statvfs(mp.c_str(), &sv) != 0) continue;
        uint64_t total = (uint64_t)sv.f_frsize * sv.f_blocks;
        uint64_t freeB = (uint64_t)sv.f_frsize * sv.f_bavail;
        char line[512];
        snprintf(line, sizeof(line), "%s -> %s 总 %s，可用 %s（%s）",
                 dev.c_str(), mp.c_str(), FormatBytes(total).c_str(),
                 FormatBytes(freeB).c_str(), devToFstype[dev].c_str());
        if (!out.empty()) out += "\n";
        out += line;
    }
    return out.empty() ? "无" : out;
}

struct CpuSample {
    unsigned long long idle = 0;
    unsigned long long total = 0;
    bool valid = false;
};
CpuSample g_prevCpu{};

double CpuUsagePercent() {
    std::string content;
    if (!ReadSmallFile("/proc/stat", content)) return 0.0;
    std::string first = SplitLines(content).front();
    if (first.rfind("cpu ", 0) != 0) return 0.0;
    // cpu user nice system idle iowait irq softirq steal ...
    std::vector<unsigned long long> v;
    size_t start = 4;
    while (start <= first.size()) {
        size_t sp = first.find(' ', start);
        std::string tok = first.substr(start, sp == std::string::npos ? std::string::npos : sp - start);
        if (!tok.empty()) v.push_back(strtoull(tok.c_str(), nullptr, 10));
        if (sp == std::string::npos) break;
        start = sp + 1;
    }
    if (v.size() < 4) return 0.0;
    unsigned long long idle = v[3] + (v.size() > 4 ? v[4] : 0);
    unsigned long long total = 0;
    for (auto x : v) total += x;
    if (!g_prevCpu.valid) {
        g_prevCpu = { idle, total, true };
        return 0.0;
    }
    unsigned long long idleDelta = idle - g_prevCpu.idle;
    unsigned long long totalDelta = total - g_prevCpu.total;
    g_prevCpu = { idle, total, true };
    if (totalDelta == 0) return 0.0;
    double busy = (double)(totalDelta - idleDelta) / (double)totalDelta * 100.0;
    if (busy < 0.0) busy = 0.0;
    if (busy > 100.0) busy = 100.0;
    return busy;
}

} // namespace

void CollectHardwareInfo(std::vector<SysEntry>& out) {
    out.clear();
    uint64_t memTotalBytes = 0;
    {
        std::string content;
        if (ReadSmallFile("/proc/meminfo", content)) {
            for (const auto& line : SplitLines(content)) {
                if (line.rfind("MemTotal:", 0) == 0) {
                    memTotalBytes = ParseMemValue(Trim(line.substr(9)));
                }
            }
        }
    }
    out.push_back({ "操作系统", CollectOs() });
    out.push_back({ "内核", CollectKernel() });
    out.push_back({ "主机名", CollectHostname() });
    out.push_back({ "处理器", CollectCpu() });
    out.push_back({ "内存总量", memTotalBytes ? FormatBytes(memTotalBytes) : "未知" });
    out.push_back({ "显卡", CollectGpu() });

    std::string mfr = CollectDmi("board_vendor");
    std::string board = CollectDmi("board_name");
    std::string model = CollectDmi("product_name");
    std::string vendor = CollectDmi("sys_vendor");
    out.push_back({ "主板", mfr + (mfr.empty() || board.empty() ? "" : " ") + board });
    out.push_back({ "电脑型号", vendor + (vendor.empty() || model.empty() ? "" : " ") + model });
    out.push_back({ "磁盘/分区", CollectDrives() });
}

void CollectLiveInfo(std::vector<SysEntry>& live) {
    live.clear();
    double cpu = CpuUsagePercent();
    uint64_t memTotal = 0, memAvail = 0;
    {
        std::string content;
        if (ReadSmallFile("/proc/meminfo", content)) {
            for (const auto& line : SplitLines(content)) {
                if (line.rfind("MemTotal:", 0) == 0) {
                    memTotal = ParseMemValue(Trim(line.substr(9)));
                } else if (line.rfind("MemAvailable:", 0) == 0) {
                    memAvail = ParseMemValue(Trim(line.substr(13)));
                }
            }
        }
    }
    char buf[256];
    snprintf(buf, sizeof(buf), "%.1f%%", cpu);
    live.push_back({ "CPU 使用率", buf });
    double usedPct = memTotal ? (double)(memTotal - memAvail) / (double)memTotal * 100.0 : 0.0;
    snprintf(buf, sizeof(buf), "%.1f%%（可用 %s）", usedPct,
             FormatBytes(memAvail).c_str());
    live.push_back({ "内存使用率", buf });
    live.push_back({ "可用物理内存", FormatBytes(memAvail) });

    std::string uptimeStr;
    {
        std::string content;
        if (ReadSmallFile("/proc/uptime", content)) {
            std::string secs = Trim(content);
            size_t sp = secs.find(' ');
            if (sp != std::string::npos) secs = secs.substr(0, sp);
            unsigned long long s = strtoull(secs.c_str(), nullptr, 10);
            snprintf(buf, sizeof(buf), "%llu 天 %llu 小时 %llu 分",
                     s / 86400, (s / 3600) % 24, (s / 60) % 60);
            uptimeStr = buf;
        }
    }
    live.push_back({ "系统运行时间", uptimeStr.empty() ? "未知" : uptimeStr });
}

void CollectTemperatures(std::vector<std::pair<std::string, double>>& out) {
    out.clear();
    std::error_code ec;
    fs::directory_iterator it("/sys/class/thermal", fs::directory_options::skip_permission_denied, ec);
    fs::directory_iterator end;
    if (ec) return;
    for (; it != end; it.increment(ec)) {
        if (ec) { ec.clear(); continue; }
        std::string name = it->path().filename().string();
        if (name.rfind("thermal_zone", 0) != 0) continue;
        std::string base = it->path().string() + "/";
        std::string type = ReadTrimmed(base + "type");
        std::string temp = ReadTrimmed(base + "temp");
        if (temp.empty()) continue;
        double millideg = strtod(temp.c_str(), nullptr);
        if (millideg <= 0) continue;
        double c = millideg / 1000.0;
        if (c < -40.0 || c > 150.0) continue;
        out.push_back({ type.empty() ? name : type, c });
    }
}
