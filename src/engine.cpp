#include "engine.h"

#include "extract.h"
#include "util.h"

#include "Everything.h"
#include "everything_ipc.h"

#include <cwchar>

namespace {

HMODULE g_dll = nullptr;
bool g_startedByUs = false;

#define LOAD_PROC(name) \
    g_##name = (decltype(&::name))GetProcAddress(g_dll, #name)

decltype(&::Everything_SetSearchW) g_Everything_SetSearchW = nullptr;
decltype(&::Everything_SetMax) g_Everything_SetMax = nullptr;
decltype(&::Everything_SetRequestFlags) g_Everything_SetRequestFlags = nullptr;
decltype(&::Everything_QueryW) g_Everything_QueryW = nullptr;
decltype(&::Everything_GetLastError) g_Everything_GetLastError = nullptr;
decltype(&::Everything_GetNumResults) g_Everything_GetNumResults = nullptr;
decltype(&::Everything_IsFolderResult) g_Everything_IsFolderResult = nullptr;
decltype(&::Everything_GetResultFullPathNameW) g_Everything_GetResultFullPathNameW = nullptr;
decltype(&::Everything_GetResultSize) g_Everything_GetResultSize = nullptr;
decltype(&::Everything_GetResultDateModified) g_Everything_GetResultDateModified = nullptr;
decltype(&::Everything_GetResultDateCreated) g_Everything_GetResultDateCreated = nullptr;
decltype(&::Everything_GetResultDateAccessed) g_Everything_GetResultDateAccessed = nullptr;
decltype(&::Everything_GetResultDateRun) g_Everything_GetResultDateRun = nullptr;
decltype(&::Everything_GetResultRunCount) g_Everything_GetResultRunCount = nullptr;
decltype(&::Everything_GetResultAttributes) g_Everything_GetResultAttributes = nullptr;
decltype(&::Everything_IsDBLoaded) g_Everything_IsDBLoaded = nullptr;
decltype(&::Everything_Exit) g_Everything_Exit = nullptr;
decltype(&::Everything_GetMajorVersion) g_Everything_GetMajorVersion = nullptr;
decltype(&::Everything_CleanUp) g_Everything_CleanUp = nullptr;

bool LoadApi(const std::wstring& dllPath) {
    if (g_dll) return true;
    g_dll = LoadLibraryW(dllPath.c_str());
    if (!g_dll) return false;
    LOAD_PROC(Everything_SetSearchW);
    LOAD_PROC(Everything_SetMax);
    LOAD_PROC(Everything_SetRequestFlags);
    LOAD_PROC(Everything_QueryW);
    LOAD_PROC(Everything_GetLastError);
    LOAD_PROC(Everything_GetNumResults);
    LOAD_PROC(Everything_IsFolderResult);
    LOAD_PROC(Everything_GetResultFullPathNameW);
    LOAD_PROC(Everything_GetResultSize);
    LOAD_PROC(Everything_GetResultDateModified);
    LOAD_PROC(Everything_GetResultDateCreated);
    LOAD_PROC(Everything_GetResultDateAccessed);
    LOAD_PROC(Everything_GetResultDateRun);
    LOAD_PROC(Everything_GetResultRunCount);
    LOAD_PROC(Everything_GetResultAttributes);
    LOAD_PROC(Everything_IsDBLoaded);
    LOAD_PROC(Everything_Exit);
    LOAD_PROC(Everything_GetMajorVersion);
    LOAD_PROC(Everything_CleanUp);

    if (!g_Everything_SetSearchW || !g_Everything_QueryW || !g_Everything_GetNumResults ||
        !g_Everything_GetResultFullPathNameW || !g_Everything_GetResultSize ||
        !g_Everything_GetResultDateModified) {
        FreeLibrary(g_dll);
        g_dll = nullptr;
        return false;
    }
    return true;
}

} // namespace

bool EngineIsLoaded() { return g_dll != nullptr; }

EngineProbeResult EngineProbe() {
    HWND wnd = FindWindowW(L"EVERYTHING_TASKBAR_NOTIFICATION", nullptr);
    if (!wnd) return EngineProbeResult::NotRunning;

    // 1) basic command responsiveness
    DWORD_PTR result = 0;
    LRESULT r = SendMessageTimeoutW(wnd, WM_USER, EVERYTHING_IPC_GET_MAJOR_VERSION, 0,
                                    SMTO_ABORTIFHUNG, 1500, &result);
    if (r == 0 && GetLastError() == ERROR_TIMEOUT) return EngineProbeResult::Hung;

    // 2) query handshake: can the engine process WM_COPYDATA queries at all?
    const wchar_t search[] = L"windowsclean_probe_nonexistent_zz";
    std::vector<unsigned char> buf(sizeof(EVERYTHING_IPC_QUERY2) + sizeof(search));
    EVERYTHING_IPC_QUERY2* query = (EVERYTHING_IPC_QUERY2*)buf.data();
    query->reply_hwnd = 0;
    query->reply_copydata_message = 0;
    query->search_flags = 0;
    query->offset = 0;
    query->max_results = 1;
    query->request_flags = 0;
    query->sort_type = EVERYTHING_IPC_SORT_NAME_ASCENDING;
    wcscpy((wchar_t*)(buf.data() + sizeof(EVERYTHING_IPC_QUERY2)), search);

    COPYDATASTRUCT cds{};
    cds.dwData = EVERYTHING_IPC_COPYDATAQUERY;
    cds.cbData = (DWORD)buf.size();
    cds.lpData = buf.data();
    result = 0;
    r = SendMessageTimeoutW(wnd, WM_COPYDATA, 0, (LPARAM)&cds,
                            SMTO_ABORTIFHUNG, 2000, &result);
    if (r == 0 && GetLastError() == ERROR_TIMEOUT) return EngineProbeResult::Hung;
    return EngineProbeResult::Ok;
}

bool EngineIsDBLoaded() {
    HWND wnd = FindWindowW(L"EVERYTHING_TASKBAR_NOTIFICATION", nullptr);
    if (!wnd) return false;
    DWORD_PTR result = 0;
    LRESULT r = SendMessageTimeoutW(wnd, WM_USER, EVERYTHING_IPC_IS_DB_LOADED, 0,
                                    SMTO_ABORTIFHUNG, 1500, &result);
    return r != 0 && result != 0;
}

bool EngineStartedByUs() { return g_startedByUs; }

bool InstallAndStartEngineIfNeeded() {
    std::wstring dir;
    if (!EnsureEverythingInstalled(dir)) {
        DbgLog(L"engine: install/extract failed, dir=%ls", dir.c_str());
        return false;
    }
    DbgLog(L"engine: files ready, dir=%ls", dir.c_str());

    if (!LoadApi(dir + L"\\everything64.dll")) {
        if (!LoadApi(GetModuleDir() + L"\\everything64.dll")) {
            DbgLog(L"engine: LoadLibrary everything64.dll failed err=%lu", GetLastError());
            return false;
        }
        DbgLog(L"engine: sdk loaded from module dir");
    } else {
        DbgLog(L"engine: sdk loaded from appdata");
    }

    EngineProbeResult pr = EngineProbe();
    DbgLog(L"engine: probe before start = %d", (int)pr);
    if (pr == EngineProbeResult::NotRunning) {
        if (!StartEverythingHidden(dir)) {
            DbgLog(L"engine: StartEverythingHidden failed err=%lu", GetLastError());
            return false;
        }
        g_startedByUs = true;
        // wait up to ~20 s for the IPC window (non-blocking probe only)
        int waited = 0;
        for (int i = 0; i < 40; ++i) {
            Sleep(500);
            pr = EngineProbe();
            if (pr != EngineProbeResult::NotRunning) break;
            waited = i + 1;
        }
        DbgLog(L"engine: probe after start = %d (waited %d x 500ms)", (int)pr, waited);
    } else if (pr == EngineProbeResult::Hung) {
        DbgLog(L"engine: an Everything instance exists but is not responding");
    }
    return pr == EngineProbeResult::Ok;
}

bool EnsureEngine() {
    return InstallAndStartEngineIfNeeded();
}

void ShutdownEngine(bool stopEngine) {
    if (g_dll) {
        if (stopEngine && g_Everything_Exit) g_Everything_Exit();
        if (g_Everything_CleanUp) g_Everything_CleanUp();
        FreeLibrary(g_dll);
        g_dll = nullptr;
    }
}

bool EngineQuery(const wchar_t* query, DWORD maxResults, DWORD* numResults, DWORD* errorOut) {
    if (!g_dll) return false;
    g_Everything_SetSearchW(query);
    if (maxResults) g_Everything_SetMax(maxResults);
    g_Everything_SetRequestFlags(EVERYTHING_REQUEST_FULL_PATH_AND_FILE_NAME |
                                 EVERYTHING_REQUEST_SIZE |
                                 EVERYTHING_REQUEST_DATE_MODIFIED |
                                 EVERYTHING_REQUEST_DATE_CREATED |
                                 EVERYTHING_REQUEST_DATE_ACCESSED |
                                 EVERYTHING_REQUEST_DATE_RUN |
                                 EVERYTHING_REQUEST_RUN_COUNT |
                                 EVERYTHING_REQUEST_ATTRIBUTES);
    BOOL ok = g_Everything_QueryW(TRUE);
    *numResults = g_Everything_GetNumResults();
    *errorOut = g_Everything_GetLastError();
    return ok != FALSE && *errorOut == EVERYTHING_OK;
}

bool EngineIsFolderResult(DWORD index) {
    return g_dll && g_Everything_IsFolderResult && g_Everything_IsFolderResult(index) != FALSE;
}

bool EngineGetFullPath(DWORD index, std::wstring& out) {
    if (!g_dll || !g_Everything_GetResultFullPathNameW) return false;
    wchar_t buf[4096];
    DWORD n = g_Everything_GetResultFullPathNameW(index, buf, (DWORD)std::size(buf));
    if (n == 0) return false;
    out.assign(buf, n);
    return true;
}

bool EngineGetSize(DWORD index, uint64_t& out) {
    if (!g_dll || !g_Everything_GetResultSize) return false;
    LARGE_INTEGER v{};
    if (!g_Everything_GetResultSize(index, &v)) return false;
    out = (uint64_t)v.QuadPart;
    return true;
}

bool EngineGetDateModified(DWORD index, FILETIME& out) {
    if (!g_dll || !g_Everything_GetResultDateModified) return false;
    return g_Everything_GetResultDateModified(index, &out) != FALSE;
}

bool EngineGetDateCreated(DWORD index, FILETIME& out) {
    if (!g_dll || !g_Everything_GetResultDateCreated) return false;
    return g_Everything_GetResultDateCreated(index, &out) != FALSE;
}

bool EngineGetDateAccessed(DWORD index, FILETIME& out) {
    if (!g_dll || !g_Everything_GetResultDateAccessed) return false;
    return g_Everything_GetResultDateAccessed(index, &out) != FALSE;
}

bool EngineGetDateRun(DWORD index, FILETIME& out) {
    if (!g_dll || !g_Everything_GetResultDateRun) return false;
    return g_Everything_GetResultDateRun(index, &out) != FALSE;
}

DWORD EngineGetRunCount(DWORD index) {
    return g_dll && g_Everything_GetResultRunCount ? g_Everything_GetResultRunCount(index) : 0;
}

DWORD EngineGetAttributes(DWORD index) {
    return g_dll && g_Everything_GetResultAttributes ? g_Everything_GetResultAttributes(index) : 0;
}

int EngineGetMajorVersion() {
    return g_dll && g_Everything_GetMajorVersion ? (int)g_Everything_GetMajorVersion() : 0;
}
