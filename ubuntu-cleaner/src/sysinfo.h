#pragma once

#include <string>
#include <utility>
#include <vector>

struct SysEntry {
    std::string name;
    std::string value;
};

// Static hardware / OS info (reads /proc, /sys, /etc; fast).
void CollectHardwareInfo(std::vector<SysEntry>& out);

// Real-time rows: CPU %, memory %, available RAM, uptime.
// CPU % measures the delta since the previous call; the first call returns 0.
void CollectLiveInfo(std::vector<SysEntry>& live);

// Reads /sys/class/thermal zones: (zone label, celsius). May be empty.
void CollectTemperatures(std::vector<std::pair<std::string, double>>& out);
