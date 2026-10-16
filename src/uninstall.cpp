#include "uninstall.h"

#include <windows.h>

#include <algorithm>

namespace {

std::wstring ReadValue(HKEY key, const wchar_t* value) {
    wchar_t buf[8192] = {};
    DWORD size = sizeof(buf);
    DWORD type = 0;
    LONG r = RegGetValueW(key, nullptr, value, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ,
                          &type, buf, &size);
    if (r != ERROR_SUCCESS) return {};
    return buf;
}

DWORD ReadDword(HKEY key, const wchar_t* value) {
    DWORD v = 0;
    DWORD size = sizeof(v);
    DWORD type = 0;
    LONG r = RegGetValueW(key, nullptr, value, RRF_RT_REG_DWORD, &type, &v, &size);
    return r == ERROR_SUCCESS ? v : 0;
}

void EnumerateRoot(HKEY root, const wchar_t* path, std::vector<AppEntry>& out) {
    HKEY hk = nullptr;
    if (RegOpenKeyExW(root, path, 0, KEY_READ | KEY_WOW64_64KEY, &hk) != ERROR_SUCCESS &&
        RegOpenKeyExW(root, path, 0, KEY_READ, &hk) != ERROR_SUCCESS) {
        return;
    }

    for (DWORD i = 0;; ++i) {
        wchar_t sub[512] = {};
        DWORD subLen = (DWORD)std::size(sub);
        if (RegEnumKeyExW(hk, i, sub, &subLen, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS)
            break;

        HKEY subKey = nullptr;
        if (RegOpenKeyExW(hk, sub, 0, KEY_READ, &subKey) != ERROR_SUCCESS) continue;

        std::wstring name = ReadValue(subKey, L"DisplayName");
        DWORD sys = ReadDword(subKey, L"SystemComponent");
        DWORD hidden = ReadDword(subKey, L"Hidden");
        if (!name.empty() && sys == 0 && hidden == 0) {
            AppEntry e;
            e.name = name;
            e.version = ReadValue(subKey, L"DisplayVersion");
            e.publisher = ReadValue(subKey, L"Publisher");
            e.installLocation = ReadValue(subKey, L"InstallLocation");
            e.uninstallString = ReadValue(subKey, L"UninstallString");
            e.url = ReadValue(subKey, L"URLInfoAbout");
            e.sizeKB = ReadDword(subKey, L"EstimatedSize");
            out.push_back(std::move(e));
        }
        RegCloseKey(subKey);
    }
    RegCloseKey(hk);
}

} // namespace

void EnumInstalledApps(std::vector<AppEntry>& out) {
    out.clear();
    EnumerateRoot(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall", out);
    EnumerateRoot(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Uninstall", out);
    EnumerateRoot(HKEY_CURRENT_USER, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall", out);

    // dedupe by uninstall string (HKLM x64 and WOW6432Node can overlap)
    std::sort(out.begin(), out.end(), [](const AppEntry& a, const AppEntry& b) {
        return a.name < b.name;
    });
    auto it = std::unique(out.begin(), out.end(), [](const AppEntry& a, const AppEntry& b) {
        return !a.uninstallString.empty() && a.uninstallString == b.uninstallString;
    });
    out.erase(it, out.end());
}
