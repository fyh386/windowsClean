#pragma once

#include <windows.h>

#include <string>

enum class EngineProbeResult {
    NotRunning,
    Hung,      // window exists but does not process messages
    Ok,
};

// Cheap responsiveness probe (SendMessageTimeout) that avoids blocking SDK
// calls when the Everything instance is wedged.
EngineProbeResult EngineProbe();

// Ensures the bundled Everything engine is installed, started and the SDK
// dll is loaded. Returns true when IPC is reachable.
bool EnsureEngine();
void ShutdownEngine(bool stopEngine);

bool EngineIsLoaded();
bool EngineIsDBLoaded();
bool EngineStartedByUs();

// Runs a blocking query; numResults receives the number of file results.
// errorOut receives EVERYTHING_OK or the SDK last error.
bool EngineQuery(const wchar_t* query, DWORD maxResults, DWORD* numResults, DWORD* errorOut);

bool EngineIsFolderResult(DWORD index);
bool EngineGetFullPath(DWORD index, std::wstring& out);
bool EngineGetSize(DWORD index, uint64_t& out);
bool EngineGetDateModified(DWORD index, FILETIME& out);
bool EngineGetDateCreated(DWORD index, FILETIME& out);
bool EngineGetDateAccessed(DWORD index, FILETIME& out);
bool EngineGetDateRun(DWORD index, FILETIME& out);
DWORD EngineGetRunCount(DWORD index);
DWORD EngineGetAttributes(DWORD index);
int EngineGetMajorVersion();

// Starts/installs the bundled engine (used by EnsureEngine).
bool InstallAndStartEngineIfNeeded();
