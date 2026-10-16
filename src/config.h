#pragma once

#include <string>
#include <vector>

struct Settings {
    bool probeEnabled = true;
    bool onlyMultiline = true;
    bool stopEverythingOnExit = false;
    bool trimUserOnly = true;
    unsigned long probeCap = 50000;
    unsigned long probeMaxFileMB = 64;
    unsigned long resultCap = 500000;
    std::vector<std::wstring> excludePaths;
};

std::wstring GetConfigPath();
bool LoadSettings(Settings& s);
bool SaveSettings(const Settings& s);
void ResetSettings(Settings& s);

