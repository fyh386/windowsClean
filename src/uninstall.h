#pragma once

#include <string>
#include <vector>

struct AppEntry {
    std::wstring name;
    std::wstring version;
    std::wstring publisher;
    std::wstring installLocation;
    std::wstring uninstallString;
    std::wstring url;
    uint64_t sizeKB = 0;
    std::wstring sizeText; // pre-formatted, retained by the list view
};

void EnumInstalledApps(std::vector<AppEntry>& out);
