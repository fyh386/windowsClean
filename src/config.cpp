#include "config.h"
#include "util.h"

namespace {
const wchar_t* kSection = L"General";

std::wstring ReadStr(const wchar_t* key, const wchar_t* def) {
    wchar_t buf[32768] = {};
    GetPrivateProfileStringW(kSection, key, def, buf, (DWORD)std::size(buf), GetConfigPath().c_str());
    return buf;
}

unsigned long ReadUlong(const wchar_t* key, unsigned long def) {
    wchar_t buf[32] = {};
    GetPrivateProfileStringW(kSection, key, L"", buf, 32, GetConfigPath().c_str());
    return buf[0] ? wcstoul(buf, nullptr, 10) : def;
}

bool ReadBool(const wchar_t* key, bool def) {
    return ReadUlong(key, def ? 1 : 0) != 0;
}
}

std::wstring GetConfigPath() {
    return GetAppDataDir() + L"\\config.ini";
}

bool LoadSettings(Settings& s) {
    s.probeEnabled = ReadBool(L"probe_enabled", true);
    s.onlyMultiline = ReadBool(L"only_multiline", true);
    s.stopEverythingOnExit = ReadBool(L"stop_everything_on_exit", false);
    s.trimUserOnly = ReadBool(L"trim_user_only", true);
    s.probeCap = ReadUlong(L"probe_cap", 50000);
    s.probeMaxFileMB = ReadUlong(L"probe_max_file_mb", 64);
    s.resultCap = ReadUlong(L"result_cap", 500000);

    s.excludePaths.clear();
    std::wstring raw = ReadStr(L"exclude_paths", L"");
    if (raw.empty()) {
        raw = L"C:\\$Recycle.Bin;C:\\System Volume Information";
    }
    size_t start = 0;
    while (start <= raw.size()) {
        size_t semi = raw.find(L';', start);
        std::wstring part = raw.substr(start, semi == std::wstring::npos ? std::wstring::npos : semi - start);
        if (!part.empty()) s.excludePaths.push_back(part);
        if (semi == std::wstring::npos) break;
        start = semi + 1;
    }
    if (s.probeCap == 0) s.probeCap = 50000;
    if (s.probeMaxFileMB == 0) s.probeMaxFileMB = 64;
    if (s.resultCap == 0) s.resultCap = 500000;
    return true;
}

bool SaveSettings(const Settings& s) {
    std::wstring ini = GetConfigPath();
    size_t pos = ini.find_last_of(L"\\");
    if (pos != std::wstring::npos) CreateDirectoryW(ini.substr(0, pos).c_str(), nullptr);

    wchar_t num[32];
    auto setU = [&](const wchar_t* key, unsigned long v) {
        swprintf_s(num, L"%lu", v);
        WritePrivateProfileStringW(kSection, key, num, ini.c_str());
    };
    auto setB = [&](const wchar_t* key, bool v) {
        WritePrivateProfileStringW(kSection, key, v ? L"1" : L"0", ini.c_str());
    };

    setB(L"probe_enabled", s.probeEnabled);
    setB(L"only_multiline", s.onlyMultiline);
    setB(L"stop_everything_on_exit", s.stopEverythingOnExit);
    setB(L"trim_user_only", s.trimUserOnly);
    setU(L"probe_cap", s.probeCap);
    setU(L"probe_max_file_mb", s.probeMaxFileMB);
    setU(L"result_cap", s.resultCap);

    std::wstring raw;
    for (const auto& p : s.excludePaths) {
        if (!raw.empty()) raw += L';';
        raw += p;
    }
    WritePrivateProfileStringW(kSection, L"exclude_paths", raw.c_str(), ini.c_str());
    return true;
}

void ResetSettings(Settings& s) {
    s = Settings();
    // Visible defaults; IsSystemExcludedPath() additionally guards these
    // locations on every drive regardless of the setting.
    s.excludePaths = { L"C:\\$Recycle.Bin", L"C:\\System Volume Information" };
}
