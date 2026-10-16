#pragma once

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

enum class ExtClass { Log, NotLog, Unknown };

ExtClass ClassifyExt(const std::wstring& extLower);
std::wstring BuildKnownLogQuery();
std::wstring BuildNoExtQuery();
std::wstring BuildUnknownExtQuery();
std::wstring BuildLargeQuery(uint64_t minSizeBytes);
std::wstring BuildArchiveQuery();
std::wstring BuildInstallerQuery();

enum class ProbeKind { Binary, Text, Unreadable };
struct ProbeOut {
    ProbeKind kind = ProbeKind::Binary;
    bool multiline = false;
};

// maxProbeBytes caps how much of the file head is read.
ProbeOut ProbeFile(const std::wstring& path, uint64_t size, uint64_t maxProbeBytes);

bool PathExcluded(const std::wstring& path, const std::vector<std::wstring>& excludes);
// Always-excluded system locations whose items cannot be usefully deleted
// (e.g. files already inside the recycle bin).
bool IsSystemExcludedPath(const std::wstring& path);
bool IsUserPath(const std::wstring& path);
bool MtimeWithinDays(const FILETIME& ft, int days);
uint64_t FileTimeToU64(const FILETIME& ft);
