#include "util.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

std::string FormatBytes(uint64_t bytes) {
    const char* units[] = { " B", " KB", " MB", " GB", " TB" };
    double v = (double)bytes;
    int u = 0;
    while (v >= 1024.0 && u < 4) { v /= 1024.0; ++u; }
    char buf[48];
    if (u == 0) snprintf(buf, sizeof(buf), "%llu%s", (unsigned long long)bytes, units[0]);
    else snprintf(buf, sizeof(buf), "%.1f%s", v, units[u]);
    return buf;
}

std::string FormatTime(time_t t) {
    struct tm tmv{};
    localtime_r(&t, &tmv);
    char buf[64];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d",
             tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
             tmv.tm_hour, tmv.tm_min);
    return buf;
}

std::string FileNameFromPath(const std::string& path) {
    size_t p = path.find_last_of('/');
    return p == std::string::npos ? path : path.substr(p + 1);
}

std::string DirFromPath(const std::string& path) {
    size_t p = path.find_last_of('/');
    if (p == std::string::npos) return "";
    if (p == 0) return "/";
    return path.substr(0, p);
}

std::string LowerAscii(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') c += 32;
    }
    return s;
}

bool StartsWithIgnoreCase(const std::string& s, const std::string& prefix) {
    if (s.size() < prefix.size()) return false;
    for (size_t i = 0; i < prefix.size(); ++i) {
        char a = s[i], b = prefix[i];
        if (a >= 'A' && a <= 'Z') a += 32;
        if (b >= 'A' && b <= 'Z') b += 32;
        if (a != b) return false;
    }
    return true;
}

bool EndsWithIgnoreCase(const std::string& s, const std::string& suffix) {
    if (s.size() < suffix.size()) return false;
    return StartsWithIgnoreCase(s.substr(s.size() - suffix.size()), suffix);
}

bool ReadSmallFile(const std::string& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

std::vector<std::string> SplitLines(const std::string& s) {
    std::vector<std::string> out;
    size_t start = 0;
    while (start <= s.size()) {
        size_t nl = s.find('\n', start);
        std::string line = s.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        out.push_back(line);
        if (nl == std::string::npos) break;
        start = nl + 1;
    }
    return out;
}

std::string Trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) --b;
    return s.substr(a, b - a);
}

std::string GetIniValue(const std::string& path, const std::string& key) {
    std::string content;
    if (!ReadSmallFile(path, content)) return "";
    for (const auto& line : SplitLines(content)) {
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        if (Trim(line.substr(0, eq)) == key) return Trim(line.substr(eq + 1));
    }
    return "";
}

void SetIniValue(const std::string& path, const std::string& key, const std::string& value) {
    std::string content;
    ReadSmallFile(path, content);
    bool replaced = false;
    std::string out;
    for (const auto& line : SplitLines(content)) {
        size_t eq = line.find('=');
        if (eq != std::string::npos && Trim(line.substr(0, eq)) == key) {
            out += key + "=" + value + "\n";
            replaced = true;
        } else if (!line.empty()) {
            out += line + "\n";
        }
    }
    if (!replaced) out += key + "=" + value + "\n";
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (f) f << out;
}
