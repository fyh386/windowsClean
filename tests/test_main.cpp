#include "config.h"
#include "filelist.h"
#include "memclean.h"
#include "recycle.h"
#include "scan.h"
#include "sysinfo.h"
#include "uninstall.h"
#include "util.h"

#include <windows.h>

#include <commctrl.h>

#include <cstdio>
#include <string>
#include <vector>

static int g_failures = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            ++g_failures;                                                    \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);           \
        }                                                                    \
    } while (0)

static std::wstring TempFile(const void* data, size_t size) {
    wchar_t dir[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, dir);
    wchar_t name[MAX_PATH] = {};
    GetTempFileNameW(dir, L"wct", 0, name);
    HANDLE h = CreateFileW(name, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(h, data, (DWORD)size, &written, nullptr);
        CloseHandle(h);
    }
    return name;
}

static void DeleteTemp(const std::wstring& p) {
    DeleteFileW(p.c_str());
}

static std::string Gbk(const wchar_t* s) {
    int n = WideCharToMultiByte(936, 0, s, -1, nullptr, 0, nullptr, nullptr);
    std::string out(n ? n - 1 : 0, '\0');
    if (n) WideCharToMultiByte(936, 0, s, -1, out.data(), n, nullptr, nullptr);
    return out;
}

static void TestClassify() {
    CHECK(ClassifyExt(L"log") == ExtClass::Log);
    CHECK(ClassifyExt(L"LOG") == ExtClass::Log);
    CHECK(ClassifyExt(L"trace") == ExtClass::Log);
    CHECK(ClassifyExt(L"etl") == ExtClass::Log);
    CHECK(ClassifyExt(L"evtx") == ExtClass::Log);
    CHECK(ClassifyExt(L"exe") == ExtClass::NotLog);
    CHECK(ClassifyExt(L"DLL") == ExtClass::NotLog);
    CHECK(ClassifyExt(L"jpg") == ExtClass::NotLog);
    CHECK(ClassifyExt(L"zip") == ExtClass::NotLog);
    CHECK(ClassifyExt(L"txt") == ExtClass::NotLog);
    CHECK(ClassifyExt(L"docx") == ExtClass::NotLog);
    CHECK(ClassifyExt(L"ldf") == ExtClass::NotLog);
    CHECK(ClassifyExt(L"abc") == ExtClass::Unknown);
    CHECK(ClassifyExt(L"") == ExtClass::Unknown);
}

static void TestQuery() {
    CHECK(BuildKnownLogQuery() == L"ext:log;trace;etl;evtx;log1;log2");
    CHECK(BuildNoExtQuery() == L"ext:");
    std::wstring uq = BuildUnknownExtQuery();
    CHECK(uq.find(L"*.*") == 0);
    CHECK(uq.find(L"!ext:exe;dll;sys") != std::wstring::npos);
    CHECK(uq.find(L"!ext:log;trace;etl;evtx;log1;log2") != std::wstring::npos);

    std::wstring lq = BuildLargeQuery(104857600);
    CHECK(lq == L"size:>=104857600");
    CHECK(BuildArchiveQuery() == L"ext:zip;7z;rar;tar;gz;bz2;xz;zst;lz4;cab;iso;wim;arj;lzh;jar;apk;ipa;deb;rpm");
    CHECK(BuildInstallerQuery() == L"ext:msi;msp;msu;exe;appx;appxbundle;msix;msixbundle;dmg;pkg;deb;rpm;apk;ipa;iso");
}

static void TestProbe() {
    // UTF-8 without BOM, multiline
    const char utf8[] = "2026-08-26 10:00:00 INFO starting\r\n2026-08-26 10:01:00 INFO done\r\n";
    std::wstring f1 = TempFile(utf8, sizeof(utf8) - 1);
    ProbeOut p1 = ProbeFile(f1, sizeof(utf8) - 1, 65536);
    CHECK(p1.kind == ProbeKind::Text);
    CHECK(p1.multiline);
    DeleteTemp(f1);

    // UTF-16LE with BOM
    const wchar_t w16[] = L"hello world\r\nsecond line\r\n";
    std::vector<unsigned char> u16;
    u16.push_back(0xFF); u16.push_back(0xFE);
    const unsigned char* wb = (const unsigned char*)w16;
    for (size_t i = 0; i < sizeof(w16) - sizeof(wchar_t); ++i) u16.push_back(wb[i]);
    std::wstring f2 = TempFile(u16.data(), u16.size());
    ProbeOut p2 = ProbeFile(f2, u16.size(), 65536);
    CHECK(p2.kind == ProbeKind::Text);
    CHECK(p2.multiline);
    DeleteTemp(f2);

    // GBK Chinese text without BOM (high bytes)
    std::string gbk = Gbk(L"测试日志第一行\r\n测试日志第二行");
    std::wstring f3 = TempFile(gbk.data(), gbk.size());
    ProbeOut p3 = ProbeFile(f3, gbk.size(), 65536);
    CHECK(p3.kind == ProbeKind::Text);
    CHECK(p3.multiline);
    DeleteTemp(f3);

    // binary with NULs
    unsigned char bin[512];
    for (int i = 0; i < 512; ++i) bin[i] = (unsigned char)(i % 251);
    bin[100] = 0;
    std::wstring f4 = TempFile(bin, sizeof(bin));
    ProbeOut p4 = ProbeFile(f4, sizeof(bin), 65536);
    CHECK(p4.kind == ProbeKind::Binary);
    DeleteTemp(f4);

    // printable ASCII, no newline -> text but not multiline
    const char one[] = "just a single line of text without any newline";
    std::wstring f5 = TempFile(one, sizeof(one) - 1);
    ProbeOut p5 = ProbeFile(f5, sizeof(one) - 1, 65536);
    CHECK(p5.kind == ProbeKind::Text);
    CHECK(!p5.multiline);
    DeleteTemp(f5);

    // empty file
    std::wstring f6 = TempFile("", 0);
    ProbeOut p6 = ProbeFile(f6, 0, 65536);
    CHECK(p6.kind == ProbeKind::Text);
    DeleteTemp(f6);

    // long path
    wchar_t dir[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, dir);
    std::wstring longDir = std::wstring(L"\\\\?\\") + dir;
    for (int i = 0; i < 20; ++i) longDir += L"wclong_segment_0123456789\\";
    // create each segment so the full long path exists
    std::wstring cur = std::wstring(L"\\\\?\\") + dir;
    for (int i = 0; i < 20; ++i) {
        cur += L"wclong_segment_0123456789\\";
        CreateDirectoryW(cur.c_str(), nullptr);
    }
    std::wstring longFile = longDir + L"longname.log.txt";
    HANDLE h = CreateFileW(longFile.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(h, utf8, sizeof(utf8) - 1, &written, nullptr);
        CloseHandle(h);
    }
    ProbeOut p7 = ProbeFile(longFile, sizeof(utf8) - 1, 65536);
    CHECK(p7.kind == ProbeKind::Text);
    DeleteFileW(longFile.c_str());
}

static void TestSort() {
    FileVec items;
    FileItem a; a.path = L"C:\\b.log"; a.size = 10; a.kind = 0;
    FileItem b; b.path = L"C:\\a.txt"; b.size = 200; b.kind = 1;
    FileItem c; c.path = L"C:\\z.log"; c.size = 100; c.kind = 0;
    items.push_back(a); items.push_back(b); items.push_back(c);

    std::vector<unsigned> order;
    BuildSortedOrder(items, order, 3, false); // size desc
    CHECK(order.size() == 3);
    CHECK(items[order[0]].size == 200);
    CHECK(items[order[1]].size == 100);
    CHECK(items[order[2]].size == 10);

    BuildSortedOrder(items, order, 2, true); // path asc
    CHECK(items[order[0]].path == L"C:\\a.txt");
    CHECK(items[order[2]].path == L"C:\\z.log");
}

static void TestFilters() {
    std::vector<std::wstring> excludes = { L"C:\\Windows\\Temp" };
    CHECK(PathExcluded(L"C:\\Windows\\Temp\\x.log", excludes));
    CHECK(!PathExcluded(L"C:\\Users\\a\\x.log", excludes));

    CHECK(GetUserProfileDir().size() > 3);
    CHECK(IsUserPath(GetUserProfileDir() + L"Documents\\x.log"));
    CHECK(!IsUserPath(L"C:\\Windows\\System32\\x.log"));

    FILETIME now{};
    GetSystemTimeAsFileTime(&now);
    CHECK(MtimeWithinDays(now, 1));
    ULARGE_INTEGER old{};
    old.LowPart = now.dwLowDateTime; old.HighPart = now.dwHighDateTime;
    old.QuadPart -= 40ULL * 24ULL * 3600ULL * 10000000ULL;
    FILETIME oldFt{};
    oldFt.dwLowDateTime = old.LowPart;
    oldFt.dwHighDateTime = old.HighPart;
    CHECK(!MtimeWithinDays(oldFt, 7));
    CHECK(MtimeWithinDays(oldFt, 60));
}

static void TestConfig() {
    Settings s;
    ResetSettings(s);
    CHECK(s.probeCap == 50000);
    CHECK(s.onlyMultiline);
    Settings loaded;
    LoadSettings(loaded); // must not throw/crash

    // exercise the status-line format used by the UI
    wchar_t buf[640];
    size_t total = 20898, probed = 50000, unreadable = 10;
    uint64_t sel = 0, ms = 123456;
    int r = swprintf_s(buf, L"共 %zu 项 · 已勾选 %llu · 合计 %s · 探测 %zu · 不可读 %zu · 耗时 %llu ms",
                       total, sel, FormatBytes(0).c_str(), probed, unreadable, ms);
    CHECK(r == 61);
    CHECK(wcslen(buf) == 61);
    CHECK(wcsstr(buf, L"耗时") != nullptr);
}

static void TestRecycleDelete() {
    const char content[] = "delete me\r\n";
    std::wstring f1 = TempFile(content, sizeof(content) - 1);
    std::wstring f2 = TempFile(content, sizeof(content) - 1);
    std::vector<std::wstring> paths = { f1, f2 };
    DeleteStats stats = DeletePathsToRecycleBin(paths, nullptr);
    CHECK(stats.ok == 2);
    CHECK(stats.failed == 0);
    CHECK(GetFileAttributesW(f1.c_str()) == INVALID_FILE_ATTRIBUTES);
    CHECK(GetFileAttributesW(f2.c_str()) == INVALID_FILE_ATTRIBUTES);
}

static void TestProcessEnum() {
    std::vector<ProcEntry> procs;
    EnumProcessEntries(procs);
    CHECK(!procs.empty());
    bool foundSelf = false;
    for (const auto& p : procs) {
        if (p.pid == GetCurrentProcessId()) foundSelf = true;
    }
    CHECK(foundSelf);
}

static void TestUninstallEnum() {
    std::vector<AppEntry> apps;
    EnumInstalledApps(apps);
    // On a real Windows machine there should be installed programs
    CHECK(!apps.empty());
    bool anyName = false;
    for (const auto& a : apps) {
        if (!a.name.empty()) anyName = true;
    }
    CHECK(anyName);
}

static void TestSysInfo() {
    std::vector<SysInfoEntry> hw;
    CollectHardwareInfo(hw);
    CHECK(!hw.empty());
    bool hasCpu = false;
    for (const auto& e : hw) {
        if (e.name == L"处理器") hasCpu = true;
    }
    CHECK(hasCpu);

    std::vector<SysInfoEntry> live;
    CollectLiveInfo(live);
    CHECK(live.size() >= 4);
    CHECK(live[0].name == L"CPU 使用率");
    CHECK(live[1].name == L"内存使用率");

    double c = 0.0;
    bool ok = QueryCpuTemperature(c); // may be unsupported; must not crash
    if (ok) CHECK(c > -40.0 && c < 150.0);
}

// Reproduces the memory-page list fill pattern (non-virtual LVS_REPORT list,
// InsertItem + ListView_SetItemText with temporary std::wstring sources).
static void TestListViewSubItemText() {
    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_LISTVIEW_CLASSES;
    InitCommonControlsEx(&icc);

    HWND wnd = CreateWindowExW(0, L"STATIC", L"", WS_POPUP, 0, 0, 400, 300,
                               nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    CHECK(wnd != nullptr);
    HWND lv = CreateWindowExW(0, WC_LISTVIEWW, L"", WS_CHILD | WS_VISIBLE | LVS_REPORT,
                              0, 0, 400, 300, wnd, nullptr, GetModuleHandleW(nullptr), nullptr);
    CHECK(lv != nullptr);

    const wchar_t* cols[] = { L"进程", L"PID", L"工作集", L"私有内存" };
    const int widths[] = { 240, 90, 120, 120 };
    for (int i = 0; i < 4; ++i) {
        LVCOLUMNW col{};
        col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM | LVCF_FMT;
        col.fmt = (i == 0) ? LVCFMT_CENTER : LVCFMT_LEFT;
        col.pszText = (LPWSTR)cols[i];
        col.cx = widths[i];
        col.iSubItem = i;
        ListView_InsertColumn(lv, i, &col);
    }

    struct Row {
        std::wstring name;
        std::wstring pidText, wsText, privText; // persistent display strings
    };
    std::vector<Row> rows = {
        { L"System", L"4", L"1.0 KB", L"512 B" },
        { L"explorer.exe", L"12345", L"941.9 MB", L"117.7 MB" },
    };

    for (size_t i = 0; i < rows.size(); ++i) {
        LVITEMW item{};
        item.mask = LVIF_TEXT | LVIF_PARAM;
        item.iItem = (int)i;
        item.lParam = (LPARAM)i;
        item.pszText = (LPWSTR)rows[i].name.c_str();
        int idx = ListView_InsertItem(lv, &item);
        CHECK(idx == (int)i);
        if (idx >= 0) {
            ListView_SetItemText(lv, idx, 1, (LPWSTR)rows[i].pidText.c_str());
            ListView_SetItemText(lv, idx, 2, (LPWSTR)rows[i].wsText.c_str());
            ListView_SetItemText(lv, idx, 3, (LPWSTR)rows[i].privText.c_str());
        }
    }

    // read back in-process; the source strings must still be alive here
    for (size_t i = 0; i < rows.size(); ++i) {
        wchar_t buf[128] = {};
        ListView_GetItemText(lv, (int)i, 1, buf, (int)std::size(buf));
        CHECK(rows[i].pidText == buf);

        wchar_t buf2[128] = {};
        ListView_GetItemText(lv, (int)i, 2, buf2, (int)std::size(buf2));
        CHECK(rows[i].wsText == buf2);

        wchar_t buf3[128] = {};
        ListView_GetItemText(lv, (int)i, 3, buf3, (int)std::size(buf3));
        CHECK(rows[i].privText == buf3);
    }

    DestroyWindow(wnd);
}

namespace {
volatile LONG g_columnClickCount = 0;
volatile LONG g_columnClickIndex = -1;

LRESULT CALLBACK HeaderTestWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_NOTIFY) {
        NMHDR* hdr = (NMHDR*)lParam;
        if (hdr->code == LVN_COLUMNCLICK) {
            InterlockedIncrement(&g_columnClickCount);
            InterlockedExchange(&g_columnClickIndex, ((NM_LISTVIEW*)lParam)->iSubItem);
        }
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}
}

// Verifies that clicking a column header on an owner-data (virtual) list view
// delivers LVN_COLUMNCLICK with the right column index.
static void TestVirtualHeaderClick() {
    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_LISTVIEW_CLASSES;
    InitCommonControlsEx(&icc);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = HeaderTestWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"WCHeaderTestWnd";
    RegisterClassExW(&wc);

    HWND wnd = CreateWindowExW(0, L"WCHeaderTestWnd", L"", WS_POPUP, 0, 0, 1000, 300,
                               nullptr, nullptr, wc.hInstance, nullptr);
    CHECK(wnd != nullptr);
    HWND lv = CreateWindowExW(0, WC_LISTVIEWW, L"", WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_OWNERDATA,
                              0, 0, 1000, 300, wnd, nullptr, wc.hInstance, nullptr);
    CHECK(lv != nullptr);

    const wchar_t* cols[] = { L"", L"名称", L"位置", L"大小", L"修改时间", L"类型" };
    const int widths[] = { 40, 240, 320, 90, 140, 110 };
    for (int i = 0; i < 6; ++i) {
        LVCOLUMNW col{};
        col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM | LVCF_FMT;
        col.pszText = (LPWSTR)cols[i];
        col.cx = widths[i];
        col.iSubItem = i;
        ListView_InsertColumn(lv, i, &col);
    }
    ListView_SetItemCount(lv, 10);

    // click the "大小" header (column 3): its client x range is the cumulative
    // width of columns 0..2 plus half of column 3.
    int x = 40 + 240 + 320 + 45;
    HWND hdr = (HWND)SendMessageW(lv, LVM_GETHEADER, 0, 0);
    CHECK(hdr != nullptr);
    LPARAM lp = (LPARAM)((15 << 16) | x);
    SendMessageW(hdr, WM_LBUTTONDOWN, (WPARAM)MK_LBUTTON, lp);
    SendMessageW(hdr, WM_LBUTTONUP, 0, lp);

    // pump messages so WM_NOTIFY reaches the parent
    MSG msg{};
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    CHECK(g_columnClickCount == 1);
    CHECK(g_columnClickIndex == 3);
    DestroyWindow(wnd);
}

// ---------------- integration (live Everything engine) ----------------

namespace {

struct EvApi {
    HMODULE dll = nullptr;
    void (WINAPI* SetSearchW)(LPCWSTR) = nullptr;
    void (WINAPI* SetMax)(DWORD) = nullptr;
    void (WINAPI* SetRequestFlags)(DWORD) = nullptr;
    BOOL (WINAPI* QueryW)(BOOL) = nullptr;
    DWORD (WINAPI* GetLastError)() = nullptr;
    DWORD (WINAPI* GetNumResults)() = nullptr;
    BOOL (WINAPI* IsDBLoaded)() = nullptr;
    BOOL (WINAPI* IsFolderResult)(DWORD) = nullptr;
    DWORD (WINAPI* GetFullPathW)(DWORD, LPWSTR, DWORD) = nullptr;
    DWORD (WINAPI* GetRunCount)(DWORD) = nullptr;
    BOOL (WINAPI* GetDateRun)(DWORD, FILETIME*) = nullptr;

    bool Load(const wchar_t* dllPath) {
        dll = LoadLibraryW(dllPath);
        if (!dll) { printf("LoadLibrary failed err=%lu\n", GetLastError()); return false; }
        SetSearchW = (void (WINAPI*)(LPCWSTR))GetProcAddress(dll, "Everything_SetSearchW");
        SetMax = (void (WINAPI*)(DWORD))GetProcAddress(dll, "Everything_SetMax");
        SetRequestFlags = (void (WINAPI*)(DWORD))GetProcAddress(dll, "Everything_SetRequestFlags");
        QueryW = (BOOL (WINAPI*)(BOOL))GetProcAddress(dll, "Everything_QueryW");
        GetLastError = (DWORD (WINAPI*)())GetProcAddress(dll, "Everything_GetLastError");
        GetNumResults = (DWORD (WINAPI*)())GetProcAddress(dll, "Everything_GetNumResults");
        IsDBLoaded = (BOOL (WINAPI*)())GetProcAddress(dll, "Everything_IsDBLoaded");
        IsFolderResult = (BOOL (WINAPI*)(DWORD))GetProcAddress(dll, "Everything_IsFolderResult");
        GetFullPathW = (DWORD (WINAPI*)(DWORD, LPWSTR, DWORD))GetProcAddress(dll, "Everything_GetResultFullPathNameW");
        GetRunCount = (DWORD (WINAPI*)(DWORD))GetProcAddress(dll, "Everything_GetResultRunCount");
        GetDateRun = (BOOL (WINAPI*)(DWORD, FILETIME*))GetProcAddress(dll, "Everything_GetResultDateRun");
        return SetSearchW && QueryW && GetLastError && GetNumResults && IsFolderResult && GetFullPathW;
    }

    DWORD Query(const wchar_t* q, DWORD* numOut) {
        SetSearchW(q);
        SetMax(1000000);
        SetRequestFlags(0x00000004 | 0x00000010 | 0x00000040 | 0x00000100); // path+size+date+attrib
        QueryW(TRUE);
        *numOut = GetNumResults();
        return GetLastError();
    }
};

} // namespace

static int RunIntegration(int argc, char** argv) {
    if (argc < 4) {
        printf("usage: %s -integration <portable-dir> <everything64.dll>\n", argv[0]);
        return 2;
    }
    std::wstring dir = Utf8ToWide(argv[2]);
    std::wstring dllPath = Utf8ToWide(argv[3]);
    bool quick = argc >= 5 && std::string(argv[4]) == "-quick";

    EvApi api;
    if (!api.Load(dllPath.c_str())) return 1;

    setvbuf(stdout, nullptr, _IONBF, 0);
    printf("probing Everything IPC...\n");
    DWORD num = 0;
    DWORD err = 0;
    bool connected = false;
    std::wstring exe = dir + L"\\Everything.exe";
    for (int i = 0; i < 240; ++i) {
        err = api.Query(L"windowsclean_probe_nonexistent_zz", &num);
        if (err == 0) { connected = true; break; }
        if (i == 0) {
            std::wstring cmd = L"\"" + exe + L"\" -startup";
            STARTUPINFOW si{};
            si.cb = sizeof(si);
            si.dwFlags = STARTF_USESHOWWINDOW;
            si.wShowWindow = SW_HIDE;
            PROCESS_INFORMATION pi{};
            CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                           nullptr, dir.c_str(), &si, &pi);
            if (pi.hProcess) {
                CloseHandle(pi.hThread);
                CloseHandle(pi.hProcess);
            }
            printf("launched Everything, waiting for IPC...\n");
        }
        Sleep(500);
    }
    if (!connected) {
        printf("FAIL: Everything IPC not reachable, last error=%lu\n", err);
        return 1;
    }
    printf("IPC connected. DB loaded: %d\n", api.IsDBLoaded() ? 1 : 0);

    if (quick) {
        struct TQ { const wchar_t* label; std::wstring q; };
        TQ qs[] = {
            { L"runcount:>0", L"runcount:>0" },
            { L"runcount:>0 exe", L"runcount:>0 ext:exe" },
            { L"archives", BuildArchiveQuery() },
            { L"installers", BuildInstallerQuery() },
            { L"large >=100MB", BuildLargeQuery(100ULL * 1024ULL * 1024ULL) },
        };
        for (const auto& q : qs) {
            ULONGLONG t0 = GetTickCount64();
            DWORD n = 0;
            DWORD e = api.Query(q.q.c_str(), &n);
            printf("%-24s err=%lu num=%lu %llu ms\n",
                   WideToUtf8(q.label).c_str(), e, n, GetTickCount64() - t0);
            if (e == 0 && n > 0 && api.GetRunCount && api.GetDateRun) {
                for (DWORD i = 0; i < n; ++i) {
                    if (api.IsFolderResult(i)) continue;
                    DWORD rc = api.GetRunCount(i);
                    FILETIME ft{};
                    BOOL ok = api.GetDateRun(i, &ft);
                    wchar_t path[1024] = {};
                    api.GetFullPathW(i, path, (DWORD)std::size(path));
                    SYSTEMTIME st{};
                    FileTimeToSystemTime(&ft, &st);
                    printf("  sample: rc=%lu date=%04d-%02d-%02d %ls\n",
                           rc, st.wYear, st.wMonth, st.wDay, path);
                    break;
                }
            }
        }
        return 0;
    }

    struct Q {
        const wchar_t* label;
        std::wstring query;
    };
    Q queries[] = {
        { L"*.exe", L"*.exe" },
        { L"ext: (extensionless)", L"ext:" },
        { L"known logs", L"ext:log;trace;etl;evtx;log1;log2" },
        { L"unknown ext", BuildUnknownExtQuery() },
        { L"size:104857600", L"size:104857600" },
        { L"runcount:>0", L"runcount:>0" },
        { L"runcount:>0 exe", L"runcount:>0 ext:exe" },
    };

    int failures = 0;
    for (const auto& q : queries) {
        ULONGLONG tq = GetTickCount64();
        DWORD n = 0;
        DWORD e = api.Query(q.query.c_str(), &n);
        ULONGLONG ms = GetTickCount64() - tq;
        wchar_t first[1024] = {};
        if (n && e == 0) {
            for (DWORD i = 0; i < n; ++i) {
                if (!api.IsFolderResult(i)) {
                    api.GetFullPathW(i, first, (DWORD)std::size(first));
                    break;
                }
            }
        }
        std::string label = WideToUtf8(q.label);
        std::string firstUtf8 = WideToUtf8(first);
        printf("%-28s err=%lu num=%lu %llu ms  first=%s\n",
               label.c_str(), e, n, ms, firstUtf8.c_str());
        if (e != 0) ++failures;
    }

    // timing experiments for the unknown-extension strategy
    {
        std::wstring shortQ = L"*.* !ext:exe;dll;sys;png";
        std::wstring fullQ = BuildUnknownExtQuery();
        std::wstring probeQ = L"*.* !ext:exe;dll;sys;png;zip;rar;7z;jpg;jpeg;png;gif;bmp;mp4;mkv;avi;mov;mp3;wav;doc;docx;xls;xlsx;ppt;pptx;pdf;txt;ini;json;xml;html;htm;css;js;py;c;cpp;h;java;sql;db;iso;msi;exe";
        struct TQ { const wchar_t* label; std::wstring q; };
        TQ tqs[] = {
            { L"*.* + short !ext (4)", shortQ },
            { L"*.* + medium !ext (40)", probeQ },
            { L"*.* + full blacklist !ext", fullQ },
        };
        for (const auto& tq : tqs) {
            ULONGLONG t0 = GetTickCount64();
            DWORD n = 0;
            DWORD e = api.Query(tq.q.c_str(), &n);
            printf("TIMING %-28s err=%lu num=%lu %llu ms\n",
                   WideToUtf8(tq.label).c_str(), e, n, GetTickCount64() - t0);
        }
    }
    if (failures) return 1;
    printf("INTEGRATION PASSED\n");
    return 0;
}

int main(int argc, char** argv) {
    if (argc >= 2 && std::string(argv[1]) == "-integration") {
        return RunIntegration(argc, argv);
    }

    TestClassify();
    TestQuery();
    TestProbe();
    TestSort();
    TestFilters();
    TestConfig();
    TestRecycleDelete();
    TestProcessEnum();
    TestUninstallEnum();
    TestSysInfo();
    TestListViewSubItemText();
    TestVirtualHeaderClick();

    if (g_failures == 0) {
        printf("ALL TESTS PASSED\n");
        return 0;
    }
    printf("%d TEST(S) FAILED\n", g_failures);
    return 1;
}
