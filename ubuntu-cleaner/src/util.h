#pragma once

#include <cstdint>
#include <ctime>
#include <string>
#include <vector>

std::string FormatBytes(uint64_t bytes);
std::string FormatTime(time_t t);
std::string FileNameFromPath(const std::string& path);
std::string DirFromPath(const std::string& path);
std::string LowerAscii(std::string s);
bool StartsWithIgnoreCase(const std::string& s, const std::string& prefix);
bool EndsWithIgnoreCase(const std::string& s, const std::string& suffix);

// Reads a whole small file into a string; returns false on failure.
bool ReadSmallFile(const std::string& path, std::string& out);
// Splits a string on '\n' and strips '\r'.
std::vector<std::string> SplitLines(const std::string& s);
std::string Trim(const std::string& s);

// Returns the value of a config key from a simple "key=value" file.
// Called from the UI thread; files are tiny.
std::string GetIniValue(const std::string& path, const std::string& key);
void SetIniValue(const std::string& path, const std::string& key, const std::string& value);
