#include "extract.h"

#include "resource.h"
#include "util.h"

#include <shellapi.h>

namespace {

bool ExtractResource(int resId, const std::wstring& destPath) {
    HRSRC hr = FindResourceW(nullptr, MAKEINTRESOURCEW(resId), RT_RCDATA);
    if (!hr) return false;
    HGLOBAL hg = LoadResource(nullptr, hr);
    if (!hg) return false;
    void* data = LockResource(hg);
    DWORD size = SizeofResource(nullptr, hr);
    if (!data || size == 0) return false;
    return WriteBinaryFile(destPath, data, size);
}

bool ResourceSize(int resId, DWORD* out) {
    HRSRC hr = FindResourceW(nullptr, MAKEINTRESOURCEW(resId), RT_RCDATA);
    if (!hr) return false;
    *out = SizeofResource(nullptr, hr);
    return *out != 0;
}

bool FileSizeMatches(const std::wstring& path, DWORD resSize) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{};
    BOOL ok = GetFileSizeEx(h, &sz);
    CloseHandle(h);
    return ok && sz.QuadPart == (LONGLONG)resSize;
}

// Writes the resource only when the destination is missing or differs in
// size. Overwriting every launch is not only slow, it fails with a sharing
// violation when a previously auto-started Everything.exe is still running
// from this folder (the default keeps it alive across app restarts).
bool ExtractIfNeeded(int resId, const std::wstring& destPath) {
    DWORD resSize = 0;
    if (ResourceSize(resId, &resSize) && FileSizeMatches(destPath, resSize)) {
        return true; // already up to date
    }
    return ExtractResource(resId, destPath);
}

} // namespace

bool EnsureEverythingInstalled(std::wstring& outDir) {
    outDir = GetAppDataDir() + L"\\Everything";
    CreateDirectoryW(outDir.c_str(), nullptr);

    std::wstring exePath = outDir + L"\\Everything.exe";
    std::wstring dllPath = outDir + L"\\everything64.dll";
    std::wstring iniPath = outDir + L"\\Everything.ini";

    bool exeOk = ExtractIfNeeded(IDR_EVERYTHING_EXE, exePath);
    bool dllOk = ExtractIfNeeded(IDR_EVERYTHING_DLL, dllPath);
    // A running Everything.exe from this folder locks both files. That is
    // expected after a previous app session left the engine alive: treat the
    // files as installed as long as they exist (the loader can still map the
    // dll and the engine answers over IPC).
    if (!exeOk && GetFileAttributesW(exePath.c_str()) == INVALID_FILE_ATTRIBUTES) return false;
    if (!dllOk && GetFileAttributesW(dllPath.c_str()) == INVALID_FILE_ATTRIBUTES) return false;

    // Portable mode: ini next to the exe. Keeps the index in the same folder.
    if (GetFileAttributesW(iniPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        const wchar_t ini[] = L"[Everything]\r\nportable=1\r\n";
        WriteBinaryFile(iniPath, ini, sizeof(ini) - sizeof(wchar_t));
    }
    return true;
}

bool StartEverythingHidden(const std::wstring& dir) {
    std::wstring exe = dir + L"\\Everything.exe";
    std::wstring cmd = L"\"" + exe + L"\" -startup";

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, dir.c_str(), &si, &pi)) {
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

bool LaunchElevatedSelf(const std::wstring& args, DWORD* exitCode) {
    wchar_t self[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, self, MAX_PATH);

    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    sei.lpVerb = L"runas";
    sei.lpFile = self;
    sei.lpParameters = args.c_str();
    sei.nShow = SW_HIDE;
    if (!ShellExecuteExW(&sei)) {
        return false;
    }
    if (sei.hProcess) {
        WaitForSingleObject(sei.hProcess, INFINITE);
        if (exitCode) {
            DWORD code = 1;
            GetExitCodeProcess(sei.hProcess, &code);
            *exitCode = code;
        }
        CloseHandle(sei.hProcess);
    }
    return true;
}
