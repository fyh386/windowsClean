#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct AppEntry {
    std::string id;       // dpkg package / snap name / flatpak application id
    std::string name;     // display name
    std::string version;
    std::string source;   // dpkg / snap / flatpak
    uint64_t sizeKB = 0;
};

// Enumerates installed software from dpkg, snap and flatpak.
void EnumInstalledApps(std::vector<AppEntry>& out);

// Uninstalls via pkexec (polkit prompt). Call off the UI thread.
bool UninstallApp(const AppEntry& app, std::string& errorOut);
