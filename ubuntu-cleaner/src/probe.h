#pragma once

#include <cstdint>
#include <string>

enum class ExtClass { Log, NotLog, Unknown };

// Lowercased extension without the dot, e.g. "log".
ExtClass ClassifyExt(const std::string& extLower);

enum class ProbeKind { Binary, Text, Unreadable };
struct ProbeOut {
    ProbeKind kind = ProbeKind::Binary;
    bool multiline = false;
};

// Reads the head of the file (up to maxProbeBytes) and guesses whether it is
// human-readable text (UTF-8/GBK-friendly heuristic, UTF-16 BOM aware).
ProbeOut ProbeFile(const std::string& path, uint64_t size, uint64_t maxProbeBytes);

bool IsArchiveExt(const std::string& extLower);
bool IsInstallerExt(const std::string& extLower);
// Lowercased final extension (without dot) of a path; "" if none.
std::string ExtOf(const std::string& path);
