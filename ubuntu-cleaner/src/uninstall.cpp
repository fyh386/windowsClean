#include "uninstall.h"

#include "util.h"

#include <cstdio>
#include <cstdlib>
#include <sys/wait.h>
#include <utility>

namespace {

std::string RunCommand(const std::string& cmd) {
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) return "";
    std::string out;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), p)) > 0) out.append(buf, n);
    pclose(p);
    return out;
}

std::string RunCommandStatus(const std::string& cmd, int& exitCode) {
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) { exitCode = -1; return ""; }
    std::string out;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), p)) > 0) out.append(buf, n);
    exitCode = pclose(p);
    if (exitCode != -1) exitCode = WEXITSTATUS(exitCode);
    return out;
}

void EnumDpkg(std::vector<AppEntry>& out) {
    std::string text = RunCommand(
        "dpkg-query -W -f='${Package}\\t${Version}\\t${Installed-Size}\\n' 2>/dev/null");
    for (const auto& line : SplitLines(text)) {
        std::vector<std::string> parts;
        size_t start = 0;
        while (start <= line.size()) {
            size_t tab = line.find('\t', start);
            parts.push_back(line.substr(start, tab == std::string::npos ? std::string::npos : tab - start));
            if (tab == std::string::npos) break;
            start = tab + 1;
        }
        if (parts.size() < 3 || parts[0].empty()) continue;
        AppEntry e;
        e.id = parts[0];
        e.name = parts[0];
        e.version = parts[1];
        e.source = "dpkg";
        e.sizeKB = strtoull(parts[2].c_str(), nullptr, 10);
        out.push_back(std::move(e));
    }
}

void EnumSnap(std::vector<AppEntry>& out) {
    std::string text = RunCommand("snap list 2>/dev/null");
    bool first = true;
    for (const auto& line : SplitLines(text)) {
        if (first) { first = false; continue; }
        std::vector<std::string> parts;
        size_t start = 0;
        while (start <= line.size()) {
            while (start < line.size() && line[start] == ' ') ++start;
            size_t sp = line.find(' ', start);
            if (sp == std::string::npos) {
                parts.push_back(line.substr(start));
                break;
            }
            parts.push_back(line.substr(start, sp - start));
            start = sp;
        }
        if (parts.size() < 2 || parts[0].empty()) continue;
        AppEntry e;
        e.id = parts[0];
        e.name = parts[0];
        e.version = parts[1];
        e.source = "snap";
        out.push_back(std::move(e));
    }
}

void EnumFlatpak(std::vector<AppEntry>& out) {
    std::string text = RunCommand(
        "flatpak list --app --columns=application,version,name 2>/dev/null");
    bool first = true;
    for (const auto& line : SplitLines(text)) {
        if (first) { first = false; continue; }
        std::vector<std::string> parts;
        size_t start = 0;
        while (start <= line.size()) {
            while (start < line.size() && line[start] == '\t') ++start;
            size_t tab = line.find('\t', start);
            if (tab == std::string::npos) {
                parts.push_back(line.substr(start));
                break;
            }
            parts.push_back(line.substr(start, tab - start));
            start = tab;
        }
        if (parts.empty() || parts[0].empty()) continue;
        AppEntry e;
        e.id = parts[0];
        e.name = parts.size() > 2 && !parts[2].empty() ? parts[2] : parts[0];
        e.version = parts.size() > 1 ? parts[1] : "";
        e.source = "flatpak";
        out.push_back(std::move(e));
    }
}

} // namespace

void EnumInstalledApps(std::vector<AppEntry>& out) {
    out.clear();
    EnumDpkg(out);
    EnumSnap(out);
    EnumFlatpak(out);
}

bool UninstallApp(const AppEntry& app, std::string& errorOut) {
    std::string cmd;
    if (app.source == "snap") {
        cmd = "pkexec snap remove '" + app.id + "' 2>&1";
    } else if (app.source == "flatpak") {
        cmd = "pkexec flatpak uninstall -y '" + app.id + "' 2>&1";
    } else {
        cmd = "pkexec apt-get remove -y '" + app.id + "' 2>&1";
    }
    int rc = 0;
    std::string out = RunCommandStatus(cmd, rc);
    errorOut = Trim(out);
    return rc == 0;
}
