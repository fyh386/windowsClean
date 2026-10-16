#include "util.h"

#include <cstdarg>
#include <cwchar>
#include <shlwapi.h>

std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring out(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), out.data(), n);
    return out;
}

std::string WideToUtf8(const std::wstring& s) {
    if (s.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring FormatBytes(uint64_t bytes) {
    const wchar_t* units[] = { L" B", L" KB", L" MB", L" GB", L" TB" };
    double v = (double)bytes;
    int u = 0;
    while (v >= 1024.0 && u < 4) { v /= 1024.0; ++u; }
    wchar_t buf[48];
    if (u == 0) swprintf_s(buf, L"%llu%s", (unsigned long long)bytes, units[0]);
    else swprintf_s(buf, L"%.1f%s", v, units[u]);
    return buf;
}

std::wstring FormatFileTime(const FILETIME& ft) {
    SYSTEMTIME st{};
    FileTimeToSystemTime(&ft, &st);
    wchar_t buf[64];
    swprintf_s(buf, L"%04d-%02d-%02d %02d:%02d",
               (int)st.wYear, (int)st.wMonth, (int)st.wDay, (int)st.wHour, (int)st.wMinute);
    return buf;
}

std::wstring FileNameFromPath(const std::wstring& path) {
    size_t p = path.find_last_of(L"\\/");
    return p == std::wstring::npos ? path : path.substr(p + 1);
}

std::wstring DirFromPath(const std::wstring& path) {
    size_t p = path.find_last_of(L"\\/");
    if (p == std::wstring::npos) return L"";
    if (p == 0) return L"\\";
    return path.substr(0, p);
}

std::wstring LowerAscii(std::wstring s) {
    for (wchar_t& c : s) {
        if (c >= L'A' && c <= L'Z') c += 32;
    }
    return s;
}

std::wstring GetModuleDir() {
    wchar_t buf[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring p = buf;
    size_t pos = p.find_last_of(L"\\");
    return pos == std::wstring::npos ? p : p.substr(0, pos);
}

std::wstring GetAppDataDir() {
    wchar_t buf[MAX_PATH] = {};
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH)) {
        GetTempPathW(MAX_PATH, buf);
    }
    std::wstring dir = buf;
    if (!dir.empty() && dir.back() != L'\\') dir += L'\\';
    dir += L"WindowsClean";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

std::wstring GetUserProfileDir() {
    wchar_t buf[MAX_PATH] = {};
    GetEnvironmentVariableW(L"USERPROFILE", buf, MAX_PATH);
    std::wstring p = buf;
    if (!p.empty() && p.back() != L'\\') p += L'\\';
    return p;
}

bool WriteBinaryFile(const std::wstring& path, const void* data, size_t size) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    bool ok = WriteFile(h, data, (DWORD)size, &written, nullptr) && written == size;
    CloseHandle(h);
    return ok;
}

bool ReadBinaryFile(const std::wstring& path, std::vector<unsigned char>& out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{};
    GetFileSizeEx(h, &sz);
    if (sz.QuadPart > (LONGLONG)(256 * 1024 * 1024)) { CloseHandle(h); return false; }
    out.resize((size_t)sz.QuadPart);
    DWORD read = 0;
    bool ok = out.empty() || (ReadFile(h, out.data(), (DWORD)out.size(), &read, nullptr) && read == out.size());
    CloseHandle(h);
    return ok;
}

bool StartsWithIgnoreCase(const std::wstring& s, const std::wstring& prefix) {
    if (prefix.size() > s.size()) return false;
    return CompareStringOrdinal(s.data(), (int)prefix.size(), prefix.data(), (int)prefix.size(), TRUE)
           == CSTR_EQUAL;
}

std::wstring AddLongPathPrefix(const std::wstring& path) {
    if (path.size() < 3) return path;
    if (path[0] == L'\\' && path[1] == L'\\' && path[2] == L'?') return path;
    if (path[1] == L':' && (path[0] >= L'A' && path[0] <= L'Z' || path[0] >= L'a' && path[0] <= L'z'))
        return L"\\\\?\\" + path;
    return path;
}

bool PathHasExtension(const std::wstring& path, std::wstring& extOut) {
    std::wstring name = FileNameFromPath(path);
    size_t dot = name.find_last_of(L'.');
    if (dot == std::wstring::npos || dot + 1 >= name.size()) {
        extOut.clear();
        return false;
    }
    extOut = LowerAscii(name.substr(dot + 1));
    return true;
}

uint64_t HashCombine64(uint64_t a, uint64_t b) {
    return a ^ (b + 0x9e3779b97f4a7c15ULL + (a << 6) + (a >> 2));
}

void DbgLog(const wchar_t* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    wchar_t line[2048];
    vswprintf_s(line, fmt, args);
    va_end(args);

    SYSTEMTIME st{};
    GetLocalTime(&st);
    wchar_t ts[64];
    swprintf_s(ts, L"%02d:%02d:%02d.%03d ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);

    std::wstring dir = GetAppDataDir();
    CreateDirectoryW(dir.c_str(), nullptr);
    HANDLE h = CreateFileW((dir + L"\\debug.log").c_str(), FILE_APPEND_DATA,
                           FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    std::string text = WideToUtf8(ts) + WideToUtf8(line) + "\r\n";
    DWORD written = 0;
    WriteFile(h, text.data(), (DWORD)text.size(), &written, nullptr);
    CloseHandle(h);
}
