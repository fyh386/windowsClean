#include "app.h"

#include "engine.h"
#include "memclean.h"
#include "recycle.h"
#include "util.h"

#include <commctrl.h>
#include <objbase.h>
#include <shellapi.h>

#include <string>

namespace {

int RunPurgeStandby() {
    return PurgeStandbyList() ? 0 : 1;
}

int RunTrim() {
    TrimWorkingSets(false);
    return 0;
}

int RunKill(DWORD pid) {
    return KillProcess(pid) ? 0 : 1;
}

int RunDeleteList(const std::wstring& listFile, const std::wstring& resultFile) {
    std::vector<std::wstring> paths;
    if (!ReadPathListFile(listFile, paths)) return 2;
    DeleteStats stats = DeletePathsToRecycleBin(paths, nullptr);
    std::string text = "ok=" + std::to_string(stats.ok) +
                       " failed=" + std::to_string(stats.failed) +
                       " permanent=" + std::to_string(stats.permanent);
    if (!resultFile.empty()) {
        WriteBinaryFile(resultFile, text.data(), text.size());
    }
    return stats.failed ? 1 : 0;
}

std::wstring Arg(int i, int argc, LPWSTR* argv) {
    return (i < argc && argv[i]) ? argv[i] : L"";
}

} // namespace

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int) {
    DbgLog(L"app start");
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);

    if (argc >= 2) {
        std::wstring a1 = LowerAscii(Arg(1, argc, argv));
        if (a1 == L"--purge-standby") { LocalFree(argv); return RunPurgeStandby(); }
        if (a1 == L"--trim") { LocalFree(argv); return RunTrim(); }
        if (a1 == L"--kill" && argc >= 3) {
            DWORD pid = wcstoul(Arg(2, argc, argv).c_str(), nullptr, 10);
            LocalFree(argv);
            return RunKill(pid);
        }
        if (a1 == L"--delete-list" && argc >= 3) {
            std::wstring resultFile;
            for (int i = 3; i + 1 < argc; ++i) {
                if (LowerAscii(Arg(i, argc, argv)) == L"--result") {
                    resultFile = Arg(i + 1, argc, argv);
                    break;
                }
            }
            std::wstring listFile = Arg(2, argc, argv);
            LocalFree(argv);
            return RunDeleteList(listFile, resultFile);
        }
    }
    LocalFree(argv);

    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\WindowsClean_SingleInstance");
    if (!mutex || GetLastError() == ERROR_ALREADY_EXISTS) {
        MessageBoxW(nullptr, L"WindowsClean 已在运行。", L"提示", MB_OK | MB_ICONINFORMATION);
        if (mutex) CloseHandle(mutex);
        return 0;
    }

    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_WIN95_CLASSES | ICC_BAR_CLASSES | ICC_LISTVIEW_CLASSES |
                ICC_TAB_CLASSES | ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);

    if (!RegisterMainWindowClass(hInstance)) {
        CloseHandle(mutex);
        return 1;
    }
    if (!CreateMainWindow(hInstance)) {
        CloseHandle(mutex);
        return 1;
    }

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    CloseHandle(mutex);
    return (int)msg.wParam;
}
