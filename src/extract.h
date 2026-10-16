#pragma once

#include <windows.h>

#include <string>

// Extracts bundled resources (Everything.exe, everything64.dll) into the app
// data folder and creates Everything.ini. Returns the engine directory.
bool EnsureEverythingInstalled(std::wstring& outDir);
bool StartEverythingHidden(const std::wstring& dir);
bool LaunchElevatedSelf(const std::wstring& args, DWORD* exitCode);
