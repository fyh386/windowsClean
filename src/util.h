#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

std::wstring Utf8ToWide(const std::string& s);
std::string WideToUtf8(const std::wstring& s);

std::wstring FormatBytes(uint64_t bytes);
std::wstring FormatFileTime(const FILETIME& ft);
std::wstring FileNameFromPath(const std::wstring& path);
std::wstring DirFromPath(const std::wstring& path);
std::wstring LowerAscii(std::wstring s);

std::wstring GetModuleDir();
std::wstring GetAppDataDir();
std::wstring GetUserProfileDir();

bool WriteBinaryFile(const std::wstring& path, const void* data, size_t size);
bool ReadBinaryFile(const std::wstring& path, std::vector<unsigned char>& out);

bool StartsWithIgnoreCase(const std::wstring& s, const std::wstring& prefix);
std::wstring AddLongPathPrefix(const std::wstring& path);
bool PathHasExtension(const std::wstring& path, std::wstring& extOut); // ext lowercased, without dot

uint64_t HashCombine64(uint64_t a, uint64_t b);

// Appends a timestamped UTF-8 line to %LOCALAPPDATA%\WindowsClean\debug.log
// (used for diagnosing lifecycle issues; tiny and always-on).
void DbgLog(const wchar_t* fmt, ...);
