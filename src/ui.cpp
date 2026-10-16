#include "app.h"

#include "engine.h"
#include "extract.h"
#include "memclean.h"
#include "recycle.h"
#include "scan.h"
#include "sysinfo.h"
#include "uninstall.h"

#include <commctrl.h>
#include <shellapi.h>
#include <algorithm>
#include <atomic>
#include <thread>
#include <unordered_set>

Settings g_settings;
HWND g_mainHwnd = nullptr;
PageData g_logPage;
PageData g_largePage;
PageData g_archivePage;
PageData g_installerPage;
PageData g_idlePage;
std::mutex g_dataMutex;

namespace {

HINSTANCE g_hInst = nullptr;
HWND g_tab = nullptr;
HFONT g_uiFont = nullptr;
HIMAGELIST g_stateImages = nullptr;
int g_activeTab = 0;
std::atomic<bool> g_engineAvailable{ false };
bool g_scanRunning = false; // UI thread only: one scan at a time

struct LogCtrls {
    HWND scope = nullptr, time = nullptr, multiline = nullptr, probe = nullptr;
    HWND refresh = nullptr, selAll = nullptr, selNone = nullptr, selInv = nullptr;
    HWND del = nullptr, openFolder = nullptr, list = nullptr, status = nullptr;
    HWND progress = nullptr;
} g_logC;

struct LargeCtrls {
    HWND threshold = nullptr, custom = nullptr, refresh = nullptr;
    HWND selAll = nullptr, selNone = nullptr, selInv = nullptr;
    HWND del = nullptr, openFolder = nullptr, list = nullptr, status = nullptr;
    HWND progress = nullptr;
} g_largeC;

struct ArchiveCtrls {
    HWND refresh = nullptr;
    HWND selAll = nullptr, selNone = nullptr, selInv = nullptr;
    HWND del = nullptr, openFolder = nullptr, list = nullptr, status = nullptr;
    HWND progress = nullptr;
} g_archiveC;

struct InstallerCtrls {
    HWND refresh = nullptr;
    HWND selAll = nullptr, selNone = nullptr, selInv = nullptr;
    HWND del = nullptr, openFolder = nullptr, list = nullptr, status = nullptr;
    HWND progress = nullptr;
} g_installerC;

struct MemCtrls {
    HWND info = nullptr, purge = nullptr, trim = nullptr, refresh = nullptr;
    HWND userOnly = nullptr, list = nullptr, end = nullptr;
} g_memC;

struct SysInfoCtrls {
    HWND refresh = nullptr, list = nullptr;
} g_sysC;

struct AppCtrls {
    HWND list = nullptr, uninstall = nullptr, openFolder = nullptr, openUrl = nullptr;
    HWND refresh = nullptr, sysset = nullptr, status = nullptr;
} g_appC;

struct IdleCtrls {
    HWND mode = nullptr, days = nullptr, custom = nullptr, refresh = nullptr;
    HWND selAll = nullptr, selNone = nullptr, selInv = nullptr;
    HWND del = nullptr, openFolder = nullptr, list = nullptr, status = nullptr;
    HWND progress = nullptr;
} g_idleC;

struct SetCtrls {
    HWND probe = nullptr, multiline = nullptr, stop = nullptr, userOnly = nullptr;
    HWND cap = nullptr, maxmb = nullptr, resultcap = nullptr, exclude = nullptr;
    HWND save = nullptr, reset = nullptr, about = nullptr;
    HWND lblCap = nullptr, lblMaxmb = nullptr, lblResult = nullptr, lblExclude = nullptr;
} g_setC;

std::vector<AppEntry> g_apps;
std::vector<ProcEntry> g_procs;

struct ScanCtx {
    int page = 0;
    int scope = 0;      // 0 all, 1 user
    int timeDays = 0;   // 0 all, 1, 7, 30
    bool probe = true;
    uint64_t largeMinBytes = 100ULL * 1024ULL * 1024ULL;
    int idleMode = 0;   // 0 files, 1 programs
    int idleDays = 90;  // unused if last run is newer than this
    Settings settings;
};

struct ScanDoneMsg {
    int page = 0;
    DWORD error = 0;
    bool truncated = false;
    bool done = false;   // true when the whole scan (incl. probing) finished
    int phase = 0;       // 0 base, 1 after extensionless probe, 2 after unknown-ext probe
    size_t total = 0;
    size_t probed = 0;
    size_t unreadable = 0;
    uint64_t elapsedMs = 0;
};

struct DeleteCtx {
    int page = 0;
    std::vector<std::wstring> paths;
    uint64_t totalBytes = 0;
};

struct ElevCtx {
    std::wstring args;
    std::wstring resultFile;
    std::wstring resultText;
    int kind = 0; // 0 delete retry, 1 purge standby, 2 kill
    DWORD pid = 0;
    int page = 0;
    std::vector<std::wstring> failedPaths;
};

struct AppsDoneMsg {
    std::vector<AppEntry> apps;
};

struct TempResult {
    bool ok = false;
    double celsius = 0.0;
};

std::vector<SysInfoEntry> g_sysRows;
int g_liveStart = 0;
bool g_sysInfoReady = false;
bool g_sysTempQueried = false;

int Dpi(HWND h) {
    return GetDpiForWindow(h);
}

int Scale(HWND h, int v) {
    return MulDiv(v, Dpi(h), 96);
}

void MoveCtrl(HWND c, int x, int y, int w, int h) {
    SetWindowPos(c, nullptr, x, y, w, h, SWP_NOZORDER);
}

HWND MakeButton(HWND parent, int id, const wchar_t* text, int x, int y, int w, int h) {
    return CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                           x, y, w, h, parent, (HMENU)(INT_PTR)id, g_hInst, nullptr);
}

HWND MakeCheck(HWND parent, int id, const wchar_t* text, int x, int y, int w, int h) {
    return CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                           x, y, w, h, parent, (HMENU)(INT_PTR)id, g_hInst, nullptr);
}

HWND MakeCombo(HWND parent, int id, int x, int y, int w, int h) {
    return CreateWindowExW(0, WC_COMBOBOXW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                           CBS_DROPDOWNLIST | WS_VSCROLL, x, y, w, h, parent,
                           (HMENU)(INT_PTR)id, g_hInst, nullptr);
}

HWND MakeEdit(HWND parent, int id, int x, int y, int w, int h, DWORD extra = 0) {
    return CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                           WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | extra,
                           x, y, w, h, parent, (HMENU)(INT_PTR)id, g_hInst, nullptr);
}

HWND MakeStatic(HWND parent, int id, const wchar_t* text, int x, int y, int w, int h) {
    return CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE | SS_LEFT,
                           x, y, w, h, parent, (HMENU)(INT_PTR)id, g_hInst, nullptr);
}

HWND MakeList(HWND parent, int id, bool virtualMode, int x, int y, int w, int h) {
    DWORD style = WS_CHILD | WS_VISIBLE | WS_BORDER | LVS_REPORT | LVS_SHOWSELALWAYS | LVS_SINGLESEL;
    if (virtualMode) style |= LVS_OWNERDATA;
    HWND lv = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"", style,
                              x, y, w, h, parent, (HMENU)(INT_PTR)id, g_hInst, nullptr);
    ListView_SetExtendedListViewStyle(lv, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    if (virtualMode) {
        ListView_SetImageList(lv, g_stateImages, LVSIL_STATE);
        ListView_SetCallbackMask(lv, LVIS_STATEIMAGEMASK);
    }
    return lv;
}

void AddColumns(HWND lv, const wchar_t* const* titles, const int* widths, int count) {
    for (int i = 0; i < count; ++i) {
        LVCOLUMNW col{};
        col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM | LVCF_FMT;
        col.fmt = (i == 0) ? LVCFMT_CENTER : LVCFMT_LEFT;
        col.pszText = (LPWSTR)titles[i];
        col.cx = widths[i];
        col.iSubItem = i;
        ListView_InsertColumn(lv, i, &col);
    }
}

void SetHeaderSort(HWND lv, int col, bool asc) {
    HWND hdr = ListView_GetHeader(lv);
    int n = Header_GetItemCount(hdr);
    for (int i = 0; i < n; ++i) {
        HDITEMW hdi{};
        hdi.mask = HDI_FORMAT;
        Header_GetItem(hdr, i, &hdi);
        hdi.fmt &= ~(HDF_SORTUP | HDF_SORTDOWN);
        if (i == col) hdi.fmt |= asc ? HDF_SORTUP : HDF_SORTDOWN;
        Header_SetItem(hdr, i, &hdi);
    }
}

PageData& PageFor(int page) {
    if (page == PAGE_LOG) return g_logPage;
    if (page == PAGE_LARGE) return g_largePage;
    if (page == PAGE_ARCHIVE) return g_archivePage;
    if (page == PAGE_INSTALLER) return g_installerPage;
    return g_idlePage;
}

HWND ListFor(int page) {
    if (page == PAGE_LOG) return g_logC.list;
    if (page == PAGE_LARGE) return g_largeC.list;
    if (page == PAGE_ARCHIVE) return g_archiveC.list;
    if (page == PAGE_INSTALLER) return g_installerC.list;
    return g_idleC.list;
}

HWND StatusFor(int page) {
    if (page == PAGE_LOG) return g_logC.status;
    if (page == PAGE_LARGE) return g_largeC.status;
    if (page == PAGE_ARCHIVE) return g_archiveC.status;
    if (page == PAGE_INSTALLER) return g_installerC.status;
    return g_idleC.status;
}

HWND ProgressFor(int page) {
    if (page == PAGE_LOG) return g_logC.progress;
    if (page == PAGE_LARGE) return g_largeC.progress;
    if (page == PAGE_ARCHIVE) return g_archiveC.progress;
    if (page == PAGE_INSTALLER) return g_installerC.progress;
    return g_idleC.progress;
}

void SetStatusText(HWND status, const std::wstring& text) {
    if (status) SetWindowTextW(status, text.c_str());
}

void SetBusy(int page, bool busy) {
    HWND refresh = page == PAGE_LOG ? g_logC.refresh :
                   (page == PAGE_LARGE ? g_largeC.refresh :
                   (page == PAGE_ARCHIVE ? g_archiveC.refresh :
                   (page == PAGE_INSTALLER ? g_installerC.refresh : g_idleC.refresh)));
    if (refresh) EnableWindow(refresh, !busy);
    HWND del = page == PAGE_LOG ? g_logC.del :
               (page == PAGE_LARGE ? g_largeC.del :
               (page == PAGE_ARCHIVE ? g_archiveC.del :
               (page == PAGE_INSTALLER ? g_installerC.del : g_idleC.del)));
    if (del) EnableWindow(del, !busy);
    HWND prog = ProgressFor(page);
    if (prog) {
        ShowWindow(prog, busy ? SW_SHOW : SW_HIDE);
        if (busy) SendMessageW(prog, PBM_SETPOS, 0, 0);
    }
}

void SetProgress(int page, size_t done, size_t total) {
    HWND prog = ProgressFor(page);
    if (!prog) return;
    SendMessageW(prog, PBM_SETRANGE32, 0, (LPARAM)total);
    SendMessageW(prog, PBM_SETPOS, (WPARAM)done, 0);
}

void UpdateListCount(int page) {
    HWND lv = ListFor(page);
    PageData& pd = PageFor(page);
    size_t n = 0;
    {
        std::lock_guard<std::mutex> lock(g_dataMutex);
        n = pd.order.size();
    }
    ListView_SetItemCount(lv, (int)std::min<size_t>(n, INT_MAX));
    InvalidateRect(lv, nullptr, TRUE);
}

void UpdateStatusLine(int page) {
    PageData& pd = PageFor(page);
    std::wstring text;
    {
        std::lock_guard<std::mutex> lock(g_dataMutex);
        wchar_t buf[256];
        swprintf_s(buf, L"共 %zu 项 · 已勾选 %llu · 合计 %s",
                   pd.order.size(), (unsigned long long)pd.selCount,
                   FormatBytes(pd.selBytes).c_str());
        text = buf;
    }
    SetStatusText(StatusFor(page), text);
}

void CreateStateImages() {
    HIMAGELIST il = ImageList_Create(16, 16, ILC_COLOR32 | ILC_MASK, 2, 2);
    HDC screen = GetDC(nullptr);
    for (int state = 0; state < 2; ++state) {
        HDC mem = CreateCompatibleDC(screen);
        HBITMAP bmp = CreateCompatibleBitmap(screen, 16, 16);
        HGDIOBJ old = SelectObject(mem, bmp);

        RECT r{ 1, 1, 15, 15 };
        HBRUSH white = (HBRUSH)GetStockObject(WHITE_BRUSH);
        FillRect(mem, &r, white);
        FrameRect(mem, &r, (HBRUSH)GetStockObject(GRAY_BRUSH));

        if (state == 1) {
            HBRUSH blue = CreateSolidBrush(RGB(0, 120, 215));
            FillRect(mem, &r, blue);
            DeleteObject(blue);
            HPEN pen = CreatePen(PS_SOLID, 2, RGB(255, 255, 255));
            HGDIOBJ oldPen = SelectObject(mem, pen);
            MoveToEx(mem, 3, 8, nullptr);
            LineTo(mem, 7, 12);
            LineTo(mem, 13, 4);
            SelectObject(mem, oldPen);
            DeleteObject(pen);
        }

        SelectObject(mem, old);
        ImageList_Add(il, bmp, nullptr);
        DeleteObject(bmp);
        DeleteDC(mem);
    }
    ReleaseDC(nullptr, screen);
    g_stateImages = il;
}

void SetDlgFont(HWND wnd) {
    if (g_uiFont) SendMessageW(wnd, WM_SETFONT, (WPARAM)g_uiFont, TRUE);
}

// ---------------- scanning ----------------

void ScanWorker(ScanCtx* ctx) {
    uint64_t t0 = GetTickCount64();

    // Engine bootstrap must not block the UI thread (first run extracts and
    // starts Everything, which can take seconds).
    if (!g_engineAvailable.load()) {
        EngineProbeResult probe = EngineProbe();
        if (probe == EngineProbeResult::Hung) {
            auto* msg = new ScanDoneMsg();
            msg->page = ctx->page;
            msg->error = 0xFFFFFFFD;
            msg->done = true;
            PostMessageW(g_mainHwnd, WM_APP_SCAN_DONE, 0, (LPARAM)msg);
            delete ctx;
            return;
        }
        DbgLog(L"ensure engine begin");
        g_engineAvailable.store(EnsureEngine());
        DbgLog(L"ensure engine done ok=%d", g_engineAvailable.load() ? 1 : 0);
        if (!g_engineAvailable.load()) {
            auto* msg = new ScanDoneMsg();
            msg->page = ctx->page;
            msg->error = 0xFFFFFFFE;
            msg->done = true;
            PostMessageW(g_mainHwnd, WM_APP_SCAN_DONE, 0, (LPARAM)msg);
            delete ctx;
            return;
        }
    }

    // Do not run a blocking query while Everything is still building its
    // index; poll IsDBLoaded (a quick command) and report progress instead.
    bool dbLoaded = EngineIsDBLoaded();
    if (!dbLoaded) {
        int waited = 0;
        for (; waited < 120; ++waited) {
            dbLoaded = EngineIsDBLoaded();
            if (dbLoaded) break;
            if (waited % 5 == 0) {
                wchar_t buf[160];
                swprintf_s(buf, L"正在等待 Everything 建立索引… %d 秒", waited);
                SetStatusText(StatusFor(ctx->page), buf);
            }
            Sleep(1000);
        }
        if (!dbLoaded) {
            auto* msg = new ScanDoneMsg();
            msg->page = ctx->page;
            msg->error = 0xFFFFFFFC; // index not ready
            msg->done = true;
            PostMessageW(g_mainHwnd, WM_APP_SCAN_DONE, 0, (LPARAM)msg);
            delete ctx;
            return;
        }
    }

    FileVec items;
    FileVec candidates;
    bool truncated = false;
    DWORD err = 0;
    size_t probedTotal = 0;
    size_t unreadableTotal = 0;

    // Reads one Everything query into items (kind known) or candidates (kind 1).
    auto readQuery = [&](const std::wstring& query, DWORD maxRead, uint8_t kind,
                         bool asCandidate) -> DWORD {
        DWORD num = 0, qerr = 0;
        if (!EngineQuery(query.c_str(), maxRead, &num, &qerr) || qerr != 0)
            return qerr ? qerr : 0xFFFFFFFF;
        DWORD n = std::min(num, maxRead);
        if (num > maxRead) truncated = true;
        for (DWORD i = 0; i < n; ++i) {
            if (EngineIsFolderResult(i)) continue;
            FileItem fi;
            if (!EngineGetFullPath(i, fi.path)) continue;
            if (PathExcluded(fi.path, ctx->settings.excludePaths)) continue;
            if (IsSystemExcludedPath(fi.path)) continue;
            if (ctx->scope == 1 && !IsUserPath(fi.path)) continue;
            EngineGetSize(i, fi.size);
            EngineGetDateModified(i, fi.modified);
            EngineGetDateCreated(i, fi.created);
            fi.attrib = EngineGetAttributes(i);
            if (!MtimeWithinDays(fi.modified, ctx->timeDays)) continue;

            if (asCandidate) {
                fi.kind = 1;
                candidates.push_back(std::move(fi));
            } else {
                fi.kind = kind;
                items.push_back(std::move(fi));
            }
        }
        return 0;
    };

    auto probeCandidates = [&](FileVec& src) -> size_t {
        if (src.empty()) return 0;
        size_t budget = std::min(src.size(), (size_t)ctx->settings.probeCap);
        std::vector<unsigned char> keep(budget, 0);
        std::atomic<size_t> next{ 0 };
        std::atomic<size_t> done{ 0 };
        std::atomic<size_t> unread{ 0 };
        uint64_t maxBytes = (uint64_t)ctx->settings.probeMaxFileMB * 1024ULL * 1024ULL;

        auto probeOne = [&]() {
            for (;;) {
                size_t idx = next.fetch_add(1);
                if (idx >= budget) break;
                ProbeOut po = ProbeFile(src[idx].path, src[idx].size, maxBytes);
                if (po.kind == ProbeKind::Text &&
                    (!ctx->settings.onlyMultiline || po.multiline)) {
                    keep[idx] = 1;
                } else if (po.kind == ProbeKind::Unreadable) {
                    ++unread;
                }
                size_t d = done.fetch_add(1) + 1;
                if (d % 128 == 0 || d == budget) {
                    uint64_t packed = ((uint64_t)budget << 32) | d;
                    PostMessageW(g_mainHwnd, WM_APP_SCAN_PROGRESS, 0, (LPARAM)packed);
                }
            }
        };

        unsigned nThreads = 8;
        std::vector<std::thread> pool;
        for (unsigned t = 0; t < nThreads; ++t) pool.emplace_back(probeOne);
        for (auto& t : pool) t.join();

        for (size_t i = 0; i < budget; ++i) {
            if (keep[i]) items.push_back(std::move(src[i]));
        }
        unreadableTotal += unread.load();
        return budget;
    };

    auto publish = [&](int phase, bool done) {
        DbgLog(L"publish phase=%d done=%d items=%zu probed=%zu", phase, done ? 1 : 0,
               items.size(), probedTotal);
        {
            std::lock_guard<std::mutex> lock(g_dataMutex);
            PageData& pd = PageFor(ctx->page);
            pd.items = items;
            pd.checked.assign(pd.items.size(), 0);
            pd.selCount = 0;
            pd.selBytes = 0;
            if (phase == 0) {
                if (ctx->page == PAGE_LOG) { pd.sortCol = 4; pd.sortAsc = false; }
                else if (ctx->page == PAGE_IDLE) { pd.sortCol = 4; pd.sortAsc = true; }
                else { pd.sortCol = 3; pd.sortAsc = false; }
            }
            BuildSortedOrder(pd.items, pd.order, pd.sortCol, pd.sortAsc,
                             ctx->page == PAGE_IDLE);
        }
        auto* msg = new ScanDoneMsg();
        msg->page = ctx->page;
        msg->truncated = truncated;
        msg->done = done;
        msg->phase = phase;
        msg->total = items.size();
        msg->probed = probedTotal;
        msg->unreadable = unreadableTotal;
        msg->elapsedMs = GetTickCount64() - t0;
        PostMessageW(g_mainHwnd, WM_APP_SCAN_DONE, 0, (LPARAM)msg);
    };

    if (ctx->page == PAGE_LOG) {
        err = readQuery(BuildKnownLogQuery(), ctx->settings.resultCap, 0, false);
        if (err) {
            auto* msg = new ScanDoneMsg();
            msg->page = ctx->page;
            msg->error = err;
            msg->done = true;
            PostMessageW(g_mainHwnd, WM_APP_SCAN_DONE, 0, (LPARAM)msg);
            delete ctx;
            return;
        }
        publish(0, !ctx->probe);

        if (ctx->probe) {
            candidates.clear();
            err = readQuery(BuildNoExtQuery(), ctx->settings.probeCap, 1, true);
            if (!err) probedTotal += probeCandidates(candidates);
            publish(1, false);

            candidates.clear();
            err = readQuery(BuildUnknownExtQuery(), ctx->settings.probeCap, 1, true);
            if (!err) probedTotal += probeCandidates(candidates);
            publish(2, true);
        }
    } else if (ctx->page == PAGE_LARGE) {
        err = readQuery(BuildLargeQuery(ctx->largeMinBytes), ctx->settings.resultCap, 2, false);
        if (err) {
            auto* msg = new ScanDoneMsg();
            msg->page = ctx->page;
            msg->error = err;
            msg->done = true;
            PostMessageW(g_mainHwnd, WM_APP_SCAN_DONE, 0, (LPARAM)msg);
            delete ctx;
            return;
        }
        publish(0, true);
    } else if (ctx->page == PAGE_ARCHIVE) {
        err = readQuery(BuildArchiveQuery(), ctx->settings.resultCap, 4, false);
        if (err) {
            auto* msg = new ScanDoneMsg();
            msg->page = ctx->page;
            msg->error = err;
            msg->done = true;
            PostMessageW(g_mainHwnd, WM_APP_SCAN_DONE, 0, (LPARAM)msg);
            delete ctx;
            return;
        }
        publish(0, true);
    } else if (ctx->page == PAGE_INSTALLER) {
        err = readQuery(BuildInstallerQuery(), ctx->settings.resultCap, 5, false);
        if (err) {
            auto* msg = new ScanDoneMsg();
            msg->page = ctx->page;
            msg->error = err;
            msg->done = true;
            PostMessageW(g_mainHwnd, WM_APP_SCAN_DONE, 0, (LPARAM)msg);
            delete ctx;
            return;
        }
        publish(0, true);
    } else if (ctx->page == PAGE_IDLE) {
        // Files/programs with run history whose last run predates the cutoff.
        std::wstring query = L"runcount:>0";
        DWORD num = 0, qerr = 0;
        if (!EngineQuery(query.c_str(), ctx->settings.resultCap, &num, &qerr) || qerr != 0) {
            auto* msg = new ScanDoneMsg();
            msg->page = ctx->page;
            msg->error = qerr ? qerr : 0xFFFFFFFF;
            msg->done = true;
            PostMessageW(g_mainHwnd, WM_APP_SCAN_DONE, 0, (LPARAM)msg);
            delete ctx;
            return;
        }
        if (num > ctx->settings.resultCap) truncated = true;

        uint64_t cutoff = 0;
        if (ctx->idleDays > 0) {
            FILETIME now{};
            GetSystemTimeAsFileTime(&now);
            ULARGE_INTEGER nowU64{};
            nowU64.LowPart = now.dwLowDateTime;
            nowU64.HighPart = now.dwHighDateTime;
            cutoff = nowU64.QuadPart - (uint64_t)ctx->idleDays * 24ULL * 3600ULL * 10000000ULL;
        }

        for (DWORD i = 0; i < std::min(num, ctx->settings.resultCap); ++i) {
            if (EngineIsFolderResult(i)) continue;
            FileItem fi;
            if (!EngineGetFullPath(i, fi.path)) continue;
            if (PathExcluded(fi.path, ctx->settings.excludePaths)) continue;
            if (IsSystemExcludedPath(fi.path)) continue;
            EngineGetSize(i, fi.size);
            EngineGetDateModified(i, fi.modified);
            EngineGetDateCreated(i, fi.created);
            EngineGetDateRun(i, fi.dateRun);
            fi.runCount = EngineGetRunCount(i);
            fi.attrib = EngineGetAttributes(i);
            fi.kind = 3;

            if (fi.runCount == 0) continue;
            if (FileTimeToU64(fi.dateRun) == 0) continue;
            if (cutoff && FileTimeToU64(fi.dateRun) > cutoff) continue;

            if (ctx->idleMode == 1) {
                std::wstring ext;
                PathHasExtension(fi.path, ext);
                static const wchar_t* kProgExts[] = {
                    L"exe", L"com", L"scr", L"bat", L"cmd", L"msi", L"msp",
                    L"lnk", L"ps1", L"vbs", L"jar", L"appref-ms"
                };
                bool isProg = false;
                for (const wchar_t* pe : kProgExts) {
                    if (ext == pe) { isProg = true; break; }
                }
                if (!isProg) continue;
            }
            items.push_back(std::move(fi));
        }
        publish(0, true);
    }
    delete ctx;
}

// ---------------- delete ----------------

void DeleteWorker(DeleteCtx* ctx) {
    DbgLog(L"delete begin paths=%zu", ctx->paths.size());
    DeleteStats stats = DeletePathsToRecycleBin(ctx->paths, [ctx](size_t done, size_t total) {
        uint64_t packed = ((uint64_t)total << 32) | done;
        PostMessageW(g_mainHwnd, WM_APP_DELETE_PROGRESS, (WPARAM)ctx->page, (LPARAM)packed);
    });
    stats.submitted = std::move(ctx->paths);
    auto* msg = new DeleteStats(std::move(stats));
    DbgLog(L"delete done ok=%zu failed=%zu", msg->ok, msg->failed);
    PostMessageW(g_mainHwnd, WM_APP_DELETE_DONE, (WPARAM)ctx->page, (LPARAM)msg);
    delete ctx;
}

void ElevWorker(ElevCtx* ctx) {
    DWORD code = 1;
    LaunchElevatedSelf(ctx->args, &code);
    if (!ctx->resultFile.empty()) {
        std::vector<unsigned char> raw;
        if (ReadBinaryFile(ctx->resultFile, raw)) {
            ctx->resultText = Utf8ToWide(std::string((const char*)raw.data(), raw.size()));
        }
        DeleteFileW(ctx->resultFile.c_str());
    }
    PostMessageW(g_mainHwnd, WM_APP_ELEVATED_DONE, (WPARAM)code, (LPARAM)ctx);
}

// ---------------- apps ----------------

void AppsWorker(HWND hwnd) {
    auto* msg = new AppsDoneMsg();
    EnumInstalledApps(msg->apps);
    PostMessageW(hwnd, WM_APP_APPS_DONE, 0, (LPARAM)msg);
}

// ---------------- layout ----------------

void LayoutLog(int x, int y, int w, int h) {
    int m = Scale(g_mainHwnd, 8);
    int bh = Scale(g_mainHwnd, 26);
    int row = y + m;
    int cx = x + m;

    int lw = Scale(g_mainHwnd, 64);
    int cw = Scale(g_mainHwnd, 100);
    int gap = Scale(g_mainHwnd, 6);

    MoveCtrl(g_logC.scope, cx, row, cw, bh); cx += cw + gap;
    MoveCtrl(g_logC.time, cx, row, cw, bh); cx += cw + gap * 2;

    SetWindowPos(g_logC.multiline, nullptr, cx, row, Scale(g_mainHwnd, 150), bh, SWP_NOZORDER);
    cx += Scale(g_mainHwnd, 150) + gap;
    SetWindowPos(g_logC.probe, nullptr, cx, row, Scale(g_mainHwnd, 150), bh, SWP_NOZORDER);

    int btnW = Scale(g_mainHwnd, 76);
    MoveCtrl(g_logC.refresh, x + w - m - btnW, row, btnW, bh);

    int row2 = row + bh + m;
    cx = x + m;
    MoveCtrl(g_logC.selAll, cx, row2, btnW, bh); cx += btnW + gap;
    MoveCtrl(g_logC.selNone, cx, row2, btnW, bh); cx += btnW + gap;
    MoveCtrl(g_logC.selInv, cx, row2, btnW, bh); cx += btnW + gap * 2;
    MoveCtrl(g_logC.del, cx, row2, Scale(g_mainHwnd, 130), bh); cx += Scale(g_mainHwnd, 130) + gap;
    MoveCtrl(g_logC.openFolder, cx, row2, Scale(g_mainHwnd, 120), bh);

    int statusH = Scale(g_mainHwnd, 20);
    int progW = Scale(g_mainHwnd, 200);
    int listY = row2 + bh + m;
    int listH = y + h - m - listY - statusH - m;
    if (listH < 40) listH = 40;
    MoveCtrl(g_logC.list, x + m, listY, w - 2 * m, listH);

    MoveCtrl(g_logC.status, x + m, listY + listH + m, w - 2 * m - progW - m, statusH);
    MoveCtrl(g_logC.progress, x + w - m - progW, listY + listH + m, progW, statusH);
}

void LayoutLarge(int x, int y, int w, int h) {
    int m = Scale(g_mainHwnd, 8);
    int bh = Scale(g_mainHwnd, 26);
    int row = y + m;
    int cx = x + m;
    int gap = Scale(g_mainHwnd, 6);
    int btnW = Scale(g_mainHwnd, 76);

    MoveCtrl(g_largeC.threshold, cx, row, Scale(g_mainHwnd, 110), bh);
    cx += Scale(g_mainHwnd, 110) + gap;
    MoveCtrl(g_largeC.custom, cx, row, Scale(g_mainHwnd, 90), bh);
    cx += Scale(g_mainHwnd, 90) + gap * 2;
    MoveCtrl(g_largeC.refresh, x + w - m - btnW, row, btnW, bh);

    int row2 = row + bh + m;
    cx = x + m;
    MoveCtrl(g_largeC.selAll, cx, row2, btnW, bh); cx += btnW + gap;
    MoveCtrl(g_largeC.selNone, cx, row2, btnW, bh); cx += btnW + gap;
    MoveCtrl(g_largeC.selInv, cx, row2, btnW, bh); cx += btnW + gap * 2;
    MoveCtrl(g_largeC.del, cx, row2, Scale(g_mainHwnd, 130), bh); cx += Scale(g_mainHwnd, 130) + gap;
    MoveCtrl(g_largeC.openFolder, cx, row2, Scale(g_mainHwnd, 120), bh);

    int statusH = Scale(g_mainHwnd, 20);
    int progW = Scale(g_mainHwnd, 200);
    int listY = row2 + bh + m;
    int listH = y + h - m - listY - statusH - m;
    if (listH < 40) listH = 40;
    MoveCtrl(g_largeC.list, x + m, listY, w - 2 * m, listH);
    MoveCtrl(g_largeC.status, x + m, listY + listH + m, w - 2 * m - progW - m, statusH);
    MoveCtrl(g_largeC.progress, x + w - m - progW, listY + listH + m, progW, statusH);
}

void LayoutFilePage(HWND refresh, HWND selAll, HWND selNone, HWND selInv,
                    HWND del, HWND openFolder, HWND list, HWND status,
                    HWND progress, int x, int y, int w, int h) {
    int m = Scale(g_mainHwnd, 8);
    int bh = Scale(g_mainHwnd, 26);
    int row = y + m;
    int btnW = Scale(g_mainHwnd, 76);
    MoveCtrl(refresh, x + w - m - btnW, row, btnW, bh);

    int row2 = row + bh + m;
    int cx = x + m;
    int gap = Scale(g_mainHwnd, 6);
    MoveCtrl(selAll, cx, row2, btnW, bh); cx += btnW + gap;
    MoveCtrl(selNone, cx, row2, btnW, bh); cx += btnW + gap;
    MoveCtrl(selInv, cx, row2, btnW, bh); cx += btnW + gap * 2;
    MoveCtrl(del, cx, row2, Scale(g_mainHwnd, 130), bh); cx += Scale(g_mainHwnd, 130) + gap;
    MoveCtrl(openFolder, cx, row2, Scale(g_mainHwnd, 120), bh);

    int statusH = Scale(g_mainHwnd, 20);
    int progW = Scale(g_mainHwnd, 200);
    int listY = row2 + bh + m;
    int listH = y + h - m - listY - statusH - m;
    if (listH < 40) listH = 40;
    MoveCtrl(list, x + m, listY, w - 2 * m, listH);
    MoveCtrl(status, x + m, listY + listH + m, w - 2 * m - progW - m, statusH);
    MoveCtrl(progress, x + w - m - progW, listY + listH + m, progW, statusH);
}

void LayoutArchive(int x, int y, int w, int h) {
    LayoutFilePage(g_archiveC.refresh, g_archiveC.selAll, g_archiveC.selNone,
                   g_archiveC.selInv, g_archiveC.del, g_archiveC.openFolder,
                   g_archiveC.list, g_archiveC.status, g_archiveC.progress,
                   x, y, w, h);
}

void LayoutInstaller(int x, int y, int w, int h) {
    LayoutFilePage(g_installerC.refresh, g_installerC.selAll, g_installerC.selNone,
                   g_installerC.selInv, g_installerC.del, g_installerC.openFolder,
                   g_installerC.list, g_installerC.status, g_installerC.progress,
                   x, y, w, h);
}

void LayoutSysInfo(int x, int y, int w, int h) {
    int m = Scale(g_mainHwnd, 8);
    int bh = Scale(g_mainHwnd, 26);
    int btnW = Scale(g_mainHwnd, 76);
    MoveCtrl(g_sysC.refresh, x + w - m - btnW, y + m, btnW, bh);
    int listY = y + m + bh + m;
    int listH = y + h - m - listY;
    if (listH < 40) listH = 40;
    MoveCtrl(g_sysC.list, x + m, listY, w - 2 * m, listH);
}

void LayoutMemory(int x, int y, int w, int h) {
    int m = Scale(g_mainHwnd, 8);
    int bh = Scale(g_mainHwnd, 26);
    int row = y + m;
    MoveCtrl(g_memC.info, x + m, row, w - 2 * m, Scale(g_mainHwnd, 62));

    int row2 = row + Scale(g_mainHwnd, 70);
    int cx = x + m;
    int gap = Scale(g_mainHwnd, 6);
    MoveCtrl(g_memC.purge, cx, row2, Scale(g_mainHwnd, 190), bh); cx += Scale(g_mainHwnd, 190) + gap;
    MoveCtrl(g_memC.trim, cx, row2, Scale(g_mainHwnd, 110), bh); cx += Scale(g_mainHwnd, 110) + gap;
    MoveCtrl(g_memC.refresh, cx, row2, Scale(g_mainHwnd, 76), bh); cx += Scale(g_mainHwnd, 76) + gap * 2;
    MoveCtrl(g_memC.userOnly, cx, row2, Scale(g_mainHwnd, 150), bh);
    MoveCtrl(g_memC.end, x + w - m - Scale(g_mainHwnd, 90), row2, Scale(g_mainHwnd, 90), bh);

    int listY = row2 + bh + m;
    int listH = y + h - m - listY;
    if (listH < 40) listH = 40;
    MoveCtrl(g_memC.list, x + m, listY, w - 2 * m, listH);
}

void LayoutApps(int x, int y, int w, int h) {
    int m = Scale(g_mainHwnd, 8);
    int bh = Scale(g_mainHwnd, 26);
    int row = y + m;
    int cx = x + m;
    int gap = Scale(g_mainHwnd, 6);
    MoveCtrl(g_appC.uninstall, cx, row, Scale(g_mainHwnd, 110), bh); cx += Scale(g_mainHwnd, 110) + gap;
    MoveCtrl(g_appC.openFolder, cx, row, Scale(g_mainHwnd, 110), bh); cx += Scale(g_mainHwnd, 110) + gap;
    MoveCtrl(g_appC.openUrl, cx, row, Scale(g_mainHwnd, 110), bh); cx += Scale(g_mainHwnd, 110) + gap;
    MoveCtrl(g_appC.refresh, cx, row, Scale(g_mainHwnd, 76), bh); cx += Scale(g_mainHwnd, 76) + gap * 2;
    MoveCtrl(g_appC.sysset, x + w - m - Scale(g_mainHwnd, 150), row, Scale(g_mainHwnd, 150), bh);

    int statusH = Scale(g_mainHwnd, 20);
    int listY = row + bh + m;
    int listH = y + h - m - listY - statusH - m;
    if (listH < 40) listH = 40;
    MoveCtrl(g_appC.list, x + m, listY, w - 2 * m, listH);
    MoveCtrl(g_appC.status, x + m, listY + listH + m, w - 2 * m, statusH);
}

void LayoutSettings(int x, int y, int w, int h) {
    int m = Scale(g_mainHwnd, 8);
    int bh = Scale(g_mainHwnd, 24);
    int lblW = Scale(g_mainHwnd, 88);
    int ew = Scale(g_mainHwnd, 92);
    int row = y + m;
    int gap = Scale(g_mainHwnd, 6);
    int cw = w - 2 * m;

    SetWindowPos(g_setC.probe, nullptr, x + m, row, Scale(g_mainHwnd, 210), bh, SWP_NOZORDER);
    SetWindowPos(g_setC.multiline, nullptr, x + m + Scale(g_mainHwnd, 220), row, Scale(g_mainHwnd, 210), bh, SWP_NOZORDER);
    row += bh + gap;
    SetWindowPos(g_setC.stop, nullptr, x + m, row, Scale(g_mainHwnd, 210), bh, SWP_NOZORDER);
    SetWindowPos(g_setC.userOnly, nullptr, x + m + Scale(g_mainHwnd, 220), row, Scale(g_mainHwnd, 210), bh, SWP_NOZORDER);
    row += bh + gap * 2;

    int colX = x + m;
    MoveCtrl(g_setC.lblCap, colX, row + 3, lblW, bh);
    colX += lblW;
    MoveCtrl(g_setC.cap, colX, row, ew, bh);
    colX += ew + gap;
    MoveCtrl(g_setC.lblMaxmb, colX, row + 3, lblW, bh);
    colX += lblW;
    MoveCtrl(g_setC.maxmb, colX, row, ew, bh);
    colX += ew + gap;
    MoveCtrl(g_setC.lblResult, colX, row + 3, lblW, bh);
    colX += lblW;
    MoveCtrl(g_setC.resultcap, colX, row, ew, bh);
    row += bh + gap;

    MoveCtrl(g_setC.lblExclude, x + m, row, cw, Scale(g_mainHwnd, 18));
    row += Scale(g_mainHwnd, 20);
    int exH = Scale(g_mainHwnd, 110);
    MoveCtrl(g_setC.exclude, x + m, row, cw, exH);
    row += exH + gap;
    MoveCtrl(g_setC.save, x + m, row, Scale(g_mainHwnd, 90), bh);
    MoveCtrl(g_setC.reset, x + m + Scale(g_mainHwnd, 100), row, Scale(g_mainHwnd, 90), bh);
    row += bh + gap * 2;
    MoveCtrl(g_setC.about, x + m, row, cw, y + h - m - row);
}

void LayoutIdle(int x, int y, int w, int h) {
    int m = Scale(g_mainHwnd, 8);
    int bh = Scale(g_mainHwnd, 26);
    int row = y + m;
    int cx = x + m;
    int gap = Scale(g_mainHwnd, 6);

    MoveCtrl(g_idleC.mode, cx, row, Scale(g_mainHwnd, 110), bh);
    cx += Scale(g_mainHwnd, 110) + gap;
    MoveCtrl(g_idleC.days, cx, row, Scale(g_mainHwnd, 130), bh);
    cx += Scale(g_mainHwnd, 130) + gap;
    MoveCtrl(g_idleC.custom, cx, row, Scale(g_mainHwnd, 80), bh);

    int btnW = Scale(g_mainHwnd, 76);
    MoveCtrl(g_idleC.refresh, x + w - m - btnW, row, btnW, bh);

    int row2 = row + bh + m;
    cx = x + m;
    MoveCtrl(g_idleC.selAll, cx, row2, btnW, bh); cx += btnW + gap;
    MoveCtrl(g_idleC.selNone, cx, row2, btnW, bh); cx += btnW + gap;
    MoveCtrl(g_idleC.selInv, cx, row2, btnW, bh); cx += btnW + gap * 2;
    MoveCtrl(g_idleC.del, cx, row2, Scale(g_mainHwnd, 130), bh); cx += Scale(g_mainHwnd, 130) + gap;
    MoveCtrl(g_idleC.openFolder, cx, row2, Scale(g_mainHwnd, 120), bh);

    int statusH = Scale(g_mainHwnd, 20);
    int progW = Scale(g_mainHwnd, 200);
    int listY = row2 + bh + m;
    int listH = y + h - m - listY - statusH - m;
    if (listH < 40) listH = 40;
    MoveCtrl(g_idleC.list, x + m, listY, w - 2 * m, listH);
    MoveCtrl(g_idleC.status, x + m, listY + listH + m, w - 2 * m - progW - m, statusH);
    MoveCtrl(g_idleC.progress, x + w - m - progW, listY + listH + m, progW, statusH);
}

void LayoutAll(int cx, int cy) {
    if (!g_tab) return;
    int tabH = Scale(g_mainHwnd, 30);
    MoveCtrl(g_tab, 0, 0, cx, tabH);
    // Content starts below the tab; fixed offset avoids any overlap with the
    // tab control itself.
    int pageY = tabH;
    int pageH = cy - pageY;
    int pageX = 0;
    int pageW = cx;

    switch (g_activeTab) {
    case 0: LayoutLog(pageX, pageY, pageW, pageH); break;
    case 1: LayoutLarge(pageX, pageY, pageW, pageH); break;
    case 2: LayoutIdle(pageX, pageY, pageW, pageH); break;
    case 3: LayoutArchive(pageX, pageY, pageW, pageH); break;
    case 4: LayoutInstaller(pageX, pageY, pageW, pageH); break;
    case 5: LayoutMemory(pageX, pageY, pageW, pageH); break;
    case 6: LayoutSysInfo(pageX, pageY, pageW, pageH); break;
    case 7: LayoutApps(pageX, pageY, pageW, pageH); break;
    case 8: LayoutSettings(pageX, pageY, pageW, pageH); break;
    }
}

void SetPageVisibility(int tab) {
    auto setVis = [](HWND c, bool vis) {
        if (c) ShowWindow(c, vis ? SW_SHOW : SW_HIDE);
    };

    bool log = tab == 0, large = tab == 1, idle = tab == 2;
    bool archive = tab == 3, installer = tab == 4, mem = tab == 5;
    bool sysinfo = tab == 6, apps = tab == 7, settings = tab == 8;

    HWND logCtrls[] = { g_logC.scope, g_logC.time, g_logC.multiline, g_logC.probe,
                        g_logC.refresh, g_logC.selAll, g_logC.selNone, g_logC.selInv,
                        g_logC.del, g_logC.openFolder, g_logC.list, g_logC.status, g_logC.progress };
    for (HWND c : logCtrls) setVis(c, log);

    HWND largeCtrls[] = { g_largeC.threshold, g_largeC.custom, g_largeC.refresh,
                          g_largeC.selAll, g_largeC.selNone, g_largeC.selInv, g_largeC.del,
                          g_largeC.openFolder, g_largeC.list, g_largeC.status, g_largeC.progress };
    for (HWND c : largeCtrls) setVis(c, large);

    HWND archiveCtrls[] = { g_archiveC.refresh,
                            g_archiveC.selAll, g_archiveC.selNone, g_archiveC.selInv,
                            g_archiveC.del, g_archiveC.openFolder,
                            g_archiveC.list, g_archiveC.status, g_archiveC.progress };
    for (HWND c : archiveCtrls) setVis(c, archive);

    HWND installerCtrls[] = { g_installerC.refresh,
                              g_installerC.selAll, g_installerC.selNone, g_installerC.selInv,
                              g_installerC.del, g_installerC.openFolder,
                              g_installerC.list, g_installerC.status, g_installerC.progress };
    for (HWND c : installerCtrls) setVis(c, installer);

    HWND idleCtrls[] = { g_idleC.mode, g_idleC.days, g_idleC.custom, g_idleC.refresh,
                         g_idleC.selAll, g_idleC.selNone, g_idleC.selInv, g_idleC.del,
                         g_idleC.openFolder, g_idleC.list, g_idleC.status, g_idleC.progress };
    for (HWND c : idleCtrls) setVis(c, idle);

    HWND memCtrls[] = { g_memC.info, g_memC.purge, g_memC.trim, g_memC.refresh,
                        g_memC.userOnly, g_memC.list, g_memC.end };
    for (HWND c : memCtrls) setVis(c, mem);

    HWND sysCtrls[] = { g_sysC.refresh, g_sysC.list };
    for (HWND c : sysCtrls) setVis(c, sysinfo);

    HWND appCtrls[] = { g_appC.uninstall, g_appC.openFolder, g_appC.openUrl,
                        g_appC.refresh, g_appC.sysset, g_appC.list, g_appC.status };
    for (HWND c : appCtrls) setVis(c, apps);

    HWND setCtrls[] = { g_setC.probe, g_setC.multiline, g_setC.stop, g_setC.userOnly,
                        g_setC.lblCap, g_setC.cap, g_setC.lblMaxmb, g_setC.maxmb,
                        g_setC.lblResult, g_setC.resultcap, g_setC.lblExclude, g_setC.exclude,
                        g_setC.save, g_setC.reset, g_setC.about };
    for (HWND c : setCtrls) setVis(c, settings);
}

// ---------------- helpers for commands ----------------

void ToggleChecked(PageData& pd, HWND lv, int displayIdx) {
    std::lock_guard<std::mutex> lock(g_dataMutex);
    if (displayIdx < 0 || displayIdx >= (int)pd.order.size()) return;
    unsigned dataIdx = pd.order[displayIdx];
    pd.checked[dataIdx] = pd.checked[dataIdx] ? 0 : 1;
}

void SetAllChecked(PageData& pd, bool value) {
    std::lock_guard<std::mutex> lock(g_dataMutex);
    for (auto& c : pd.checked) c = value ? 1 : 0;
}

void InvertChecked(PageData& pd) {
    std::lock_guard<std::mutex> lock(g_dataMutex);
    for (auto& c : pd.checked) c = c ? 0 : 1;
}

void RecalcSelected(PageData& page) {
    std::lock_guard<std::mutex> lock(g_dataMutex);
    page.selCount = 0;
    page.selBytes = 0;
    for (size_t i = 0; i < page.checked.size(); ++i) {
        if (page.checked[i]) {
            ++page.selCount;
            page.selBytes += page.items[i].size;
        }
    }
}

std::wstring GetSelectedPath(int page) {
    HWND lv = ListFor(page);
    int idx = ListView_GetNextItem(lv, -1, LVNI_SELECTED);
    if (idx < 0) return {};
    std::lock_guard<std::mutex> lock(g_dataMutex);
    PageData& pd = PageFor(page);
    if (idx >= (int)pd.order.size()) return {};
    return pd.items[pd.order[idx]].path;
}

void OpenContainingFolder(const std::wstring& path) {
    if (path.empty()) {
        MessageBoxW(g_mainHwnd, L"请先在列表中选择一个文件。", L"提示", MB_OK | MB_ICONINFORMATION);
        return;
    }
    std::wstring args = L"/select,\"" + path + L"\"";
    ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
}

void ShowScanError(int page, DWORD err) {
    const wchar_t* text = L"查询失败（未知错误）。";
    if (err == 0xFFFFFFFC) text = L"Everything 索引仍在建立中，请稍后点击“刷新”重试。";
    if (err == 0xFFFFFFFD) text = L"Everything 引擎无响应（可能已卡死）。请重启 Everything 与本程序后重试。";
    if (err == 0xFFFFFFFE)
        text = L"无法启动 Everything 引擎：请先结束所有 Everything.exe 后点“刷新”重试；仍失败请检查杀毒软件隔离区。";
    if (err == 0xFFFFFFFF) text = L"无法连接 Everything 引擎，请点击“刷新”重试。";
    SetStatusText(StatusFor(page), text);
}

void SortPage(int page) {
    HWND lv = ListFor(page);
    PageData& pd = PageFor(page);
    {
        std::lock_guard<std::mutex> lock(g_dataMutex);
        BuildSortedOrder(pd.items, pd.order, pd.sortCol, pd.sortAsc, page == PAGE_IDLE);
    }
    SetHeaderSort(lv, pd.sortCol, pd.sortAsc);
    UpdateListCount(page);
}

void StartScan(int page) {
    PageData& pd = PageFor(page);
    if (g_scanRunning || pd.scanning) return;
    g_scanRunning = true;
    DbgLog(L"scan start page=%d", page);

    pd.scanning = true;
    SetBusy(page, true);
    SetStatusText(StatusFor(page), L"正在启动 Everything 引擎…");

    auto* ctx = new ScanCtx();
    ctx->page = page;
    if (page == PAGE_LOG) {
        ctx->scope = (int)SendMessageW(g_logC.scope, CB_GETCURSEL, 0, 0);
        if (ctx->scope == CB_ERR) ctx->scope = 0;
        int timeSel = (int)SendMessageW(g_logC.time, CB_GETCURSEL, 0, 0);
        if (timeSel == CB_ERR) timeSel = 0;
        static const int kDays[] = { 0, 1, 7, 30 };
        ctx->timeDays = kDays[std::min(timeSel, 3)];
        ctx->probe = SendMessageW(g_logC.probe, BM_GETCHECK, 0, 0) == BST_CHECKED;
    } else if (page == PAGE_IDLE) {
        ctx->scope = 0;
        ctx->timeDays = 0;
        ctx->probe = false;
        ctx->idleMode = (int)SendMessageW(g_idleC.mode, CB_GETCURSEL, 0, 0);
        if (ctx->idleMode == CB_ERR) ctx->idleMode = 0;
        int daySel = (int)SendMessageW(g_idleC.days, CB_GETCURSEL, 0, 0);
        if (daySel == CB_ERR) daySel = 1;
        static const int kIdleDays[] = { 30, 90, 180, 365, 0 };
        ctx->idleDays = kIdleDays[std::min(daySel, 4)];
        if (ctx->idleDays == 0) {
            wchar_t buf[64] = {};
            GetWindowTextW(g_idleC.custom, buf, 64);
            unsigned long v = buf[0] ? wcstoul(buf, nullptr, 10) : 90;
            if (v == 0) v = 90;
            ctx->idleDays = (int)v;
        }
    } else {
        ctx->scope = 0;
        ctx->timeDays = 0;
        ctx->probe = false;
        uint64_t mb = 100;
        int sel = (int)SendMessageW(g_largeC.threshold, CB_GETCURSEL, 0, 0);
        if (sel == 0) mb = 50;
        else if (sel == 1) mb = 100;
        else if (sel == 2) mb = 500;
        else if (sel == 3) mb = 1024;
        else if (sel == 4) {
            wchar_t buf[64] = {};
            GetWindowTextW(g_largeC.custom, buf, 64);
            unsigned long v = buf[0] ? wcstoul(buf, nullptr, 10) : 100;
            if (v == 0) v = 100;
            mb = v;
        }
        ctx->largeMinBytes = mb * 1024ULL * 1024ULL;
    }
    ctx->settings = g_settings;

    std::thread(ScanWorker, ctx).detach();
}

void StartDelete(int page) {
    PageData& pd = PageFor(page);
    if (pd.scanning) return;
    std::vector<std::wstring> paths;
    uint64_t totalBytes = 0;
    {
        std::lock_guard<std::mutex> lock(g_dataMutex);
        for (size_t i = 0; i < pd.checked.size(); ++i) {
            if (pd.checked[i]) {
                paths.push_back(pd.items[i].path);
                totalBytes += pd.items[i].size;
            }
        }
    }
    if (paths.empty()) {
        MessageBoxW(g_mainHwnd, L"请先勾选要删除的文件。", L"提示", MB_OK | MB_ICONINFORMATION);
        return;
    }

    size_t remote = 0;
    for (const auto& p : paths) {
        if (p.size() >= 2 && p[1] == L':' && GetDriveTypeW(std::wstring(1, p[0]).append(L":\\").c_str()) == DRIVE_REMOTE)
            ++remote;
    }

    wchar_t buf[512];
    if (remote == 0) {
        swprintf_s(buf, L"确定将 %zu 个文件（合计 %s）移动到回收站吗？",
                   paths.size(), FormatBytes(totalBytes).c_str());
    } else {
        swprintf_s(buf, L"确定删除 %zu 个文件（合计 %s）吗？\n\n其中 %zu 个位于网络驱动器，"
                        L"网络驱动器上的文件无法进入回收站，将被永久删除！",
                   paths.size(), FormatBytes(totalBytes).c_str(), remote);
    }
    if (MessageBoxW(g_mainHwnd, buf, L"确认删除", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES)
        return;

    auto* ctx = new DeleteCtx();
    ctx->page = page;
    ctx->paths = std::move(paths);
    ctx->totalBytes = totalBytes;
    SetBusy(page, true);
    SetStatusText(StatusFor(page), L"正在删除到回收站…");
    std::thread(DeleteWorker, ctx).detach();
}

void SortOnColumn(int page, int col) {
    PageData& pd = PageFor(page);
    {
        std::lock_guard<std::mutex> lock(g_dataMutex);
        if (pd.sortCol == col) {
            pd.sortAsc = !pd.sortAsc;
        } else {
            pd.sortCol = col;
            // Size and date columns start descending (biggest/newest first).
            pd.sortAsc = !(col == 3 || col == 4);
        }
    }
    SortPage(page);
}

void RefreshApps() {
    std::thread(AppsWorker, g_mainHwnd).detach();
}

bool g_procRefreshRunning = false; // UI thread only
bool g_procRefreshPending = false; // UI thread only

void RefreshMemInfoOnly();

void FillProcList(std::vector<ProcEntry>& procs) {
    SendMessageW(g_memC.list, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(g_memC.list);
    for (size_t i = 0; i < procs.size(); ++i) {
        ProcEntry& pr = procs[i];
        // Format into persistent members first: the list view keeps the
        // pointers and reads them later, so temporaries would go stale.
        pr.pidText = std::to_wstring(pr.pid);
        pr.wsText = FormatBytes(pr.workingSet);
        pr.privText = FormatBytes(pr.privateBytes);

        LVITEMW item{};
        item.mask = LVIF_TEXT | LVIF_PARAM;
        item.iItem = (int)i;
        item.lParam = (LPARAM)i;
        item.pszText = (LPWSTR)pr.name.c_str();
        int idx = ListView_InsertItem(g_memC.list, &item);
        if (idx >= 0) {
            ListView_SetItemText(g_memC.list, idx, 1, (LPWSTR)pr.pidText.c_str());
            ListView_SetItemText(g_memC.list, idx, 2, (LPWSTR)pr.wsText.c_str());
            ListView_SetItemText(g_memC.list, idx, 3, (LPWSTR)pr.privText.c_str());
        }
    }
    SendMessageW(g_memC.list, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(g_memC.list, nullptr, TRUE);
}

void RefreshMemory() {
    RefreshMemInfoOnly();
    if (g_procRefreshRunning) {
        g_procRefreshPending = true;
        return;
    }
    g_procRefreshRunning = true;
    std::thread([] {
        auto* procs = new std::vector<ProcEntry>();
        EnumProcessEntries(*procs);
        PostMessageW(g_mainHwnd, WM_APP_PROC_DONE, 0, (LPARAM)procs);
    }).detach();
}

void RefreshMemInfoOnly() {
    MemInfo mi{};
    if (!GetMemInfo(mi)) return;
    double usedPct = mi.totalPhys ? (1.0 - (double)mi.availPhys / mi.totalPhys) * 100.0 : 0.0;
    wchar_t buf[512];
    swprintf_s(buf,
               L"物理内存总量：%s\n可用物理内存：%s（已用 %.1f%%）\n"
               L"提交限制：%s，可用提交：%s",
               FormatBytes(mi.totalPhys).c_str(),
               FormatBytes(mi.availPhys).c_str(), usedPct,
               FormatBytes(mi.totalCommit).c_str(),
               FormatBytes(mi.availCommit).c_str());
    SetWindowTextW(g_memC.info, buf);
}

void FillSysInfoList() {
    SendMessageW(g_sysC.list, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(g_sysC.list);
    for (size_t i = 0; i < g_sysRows.size(); ++i) {
        SysInfoEntry& e = g_sysRows[i];
        LVITEMW item{};
        item.mask = LVIF_TEXT | LVIF_PARAM;
        item.iItem = (int)i;
        item.lParam = (LPARAM)i;
        item.pszText = (LPWSTR)e.name.c_str();
        int idx = ListView_InsertItem(g_sysC.list, &item);
        if (idx >= 0) ListView_SetItemText(g_sysC.list, idx, 1, (LPWSTR)e.value.c_str());
    }
    SendMessageW(g_sysC.list, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(g_sysC.list, nullptr, TRUE);
}

void StartTempQuery() {
    if (g_sysTempQueried) return;
    g_sysTempQueried = true;
    std::thread([] {
        auto* tr = new TempResult();
        tr->ok = QueryCpuTemperature(tr->celsius);
        PostMessageW(g_mainHwnd, WM_APP_SYSINFO_TEMP_DONE, 0, (LPARAM)tr);
    }).detach();
}

void PopulateSysInfo() {
    std::vector<SysInfoEntry> hw;
    CollectHardwareInfo(hw);
    std::vector<SysInfoEntry> live;
    CollectLiveInfo(live);

    g_sysRows.clear();
    for (auto& e : hw) g_sysRows.push_back(std::move(e));
    g_liveStart = (int)g_sysRows.size();
    for (auto& e : live) g_sysRows.push_back(std::move(e));
    g_sysRows.push_back({ L"CPU 温度", L"查询中…" });

    g_sysInfoReady = true;
    FillSysInfoList();
    StartTempQuery();
}

void RefreshLiveRows() {
    if (!g_sysInfoReady || g_sysRows.empty()) return;
    std::vector<SysInfoEntry> live;
    CollectLiveInfo(live);
    size_t liveRows = (size_t)std::max(0, (int)g_sysRows.size() - g_liveStart - 1);
    size_t n = std::min(live.size(), liveRows);
    for (size_t i = 0; i < n; ++i) {
        g_sysRows[g_liveStart + i].value = live[i].value;
        ListView_SetItemText(g_sysC.list, g_liveStart + (int)i, 1,
                             (LPWSTR)g_sysRows[g_liveStart + i].value.c_str());
    }
}

void SetTempRow(bool ok, double celsius) {
    if (!g_sysInfoReady || g_sysRows.empty()) return;
    std::wstring v;
    if (ok) {
        wchar_t buf[64];
        swprintf_s(buf, L"%.1f ℃", celsius);
        v = buf;
    } else {
        v = L"不可用（ACPI 不支持或传感器缺失）";
    }
    int idx = (int)g_sysRows.size() - 1;
    if (idx < 0) return;
    g_sysRows[idx].value = v;
    ListView_SetItemText(g_sysC.list, idx, 1, (LPWSTR)v.c_str());
}

void EndSelectedProcess() {
    int idx = ListView_GetNextItem(g_memC.list, -1, LVNI_SELECTED);
    if (idx < 0 || idx >= (int)g_procs.size()) {
        MessageBoxW(g_mainHwnd, L"请先选择要结束的进程。", L"提示", MB_OK | MB_ICONINFORMATION);
        return;
    }
    DWORD pid = g_procs[idx].pid;
    wchar_t buf[256];
    swprintf_s(buf, L"确定结束进程“%s”（PID %lu）吗？未保存的数据可能丢失。",
               g_procs[idx].name.c_str(), pid);
    if (MessageBoxW(g_mainHwnd, buf, L"结束进程", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES)
        return;

    if (!KillProcess(pid)) {
        if (MessageBoxW(g_mainHwnd, L"权限不足，是否以管理员身份重试？",
                        L"结束进程", MB_YESNO | MB_ICONWARNING) == IDYES) {
            auto* ctx = new ElevCtx();
            ctx->kind = 2;
            ctx->pid = pid;
            wchar_t args[64];
            swprintf_s(args, L"--kill %lu", pid);
            ctx->args = args;
            std::thread(ElevWorker, ctx).detach();
        }
        return;
    }
    RefreshMemory();
}

void SplitCommand(const std::wstring& cmd, std::wstring& file, std::wstring& params) {
    file.clear();
    params.clear();
    std::wstring c = cmd;
    size_t s = c.find_first_not_of(L" \t");
    if (s == std::wstring::npos) return;
    c = c.substr(s);
    if (c[0] == L'"') {
        size_t e = c.find(L'"', 1);
        if (e == std::wstring::npos) { file = c.substr(1); return; }
        file = c.substr(1, e - 1);
        params = c.substr(e + 1);
    } else {
        size_t e = c.find(L' ');
        if (e == std::wstring::npos) { file = c; return; }
        file = c.substr(0, e);
        params = c.substr(e + 1);
    }
}

void UninstallSelected() {
    int idx = ListView_GetNextItem(g_appC.list, -1, LVNI_SELECTED);
    if (idx < 0 || idx >= (int)g_apps.size()) {
        MessageBoxW(g_mainHwnd, L"请先选择要卸载的程序。", L"提示", MB_OK | MB_ICONINFORMATION);
        return;
    }
    const AppEntry& app = g_apps[idx];
    if (app.uninstallString.empty()) {
        ShellExecuteW(nullptr, L"open", L"ms-settings:appsfeatures", nullptr, nullptr, SW_SHOWNORMAL);
        return;
    }

    std::wstring file, params;
    SplitCommand(app.uninstallString, file, params);

    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOASYNC;
    sei.lpVerb = L"runas";
    sei.lpFile = file.c_str();
    sei.lpParameters = params.empty() ? nullptr : params.c_str();
    sei.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&sei)) {
        MessageBoxW(g_mainHwnd,
                    (L"无法启动卸载程序（" + app.uninstallString + L"）。\n已为你打开系统“应用和功能”设置。").c_str(),
                    L"卸载", MB_OK | MB_ICONWARNING);
        ShellExecuteW(nullptr, L"open", L"ms-settings:appsfeatures", nullptr, nullptr, SW_SHOWNORMAL);
        return;
    }
    SetWindowTextW(g_appC.status, (L"已启动卸载程序：" + app.name).c_str());
}

void OpenAppFolder() {
    int idx = ListView_GetNextItem(g_appC.list, -1, LVNI_SELECTED);
    if (idx < 0 || idx >= (int)g_apps.size()) return;
    const std::wstring& loc = g_apps[idx].installLocation;
    if (loc.empty()) {
        MessageBoxW(g_mainHwnd, L"该程序未提供安装位置。", L"提示", MB_OK | MB_ICONINFORMATION);
        return;
    }
    ShellExecuteW(nullptr, L"open", loc.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void OpenAppUrl() {
    int idx = ListView_GetNextItem(g_appC.list, -1, LVNI_SELECTED);
    if (idx < 0 || idx >= (int)g_apps.size()) return;
    const std::wstring& url = g_apps[idx].url;
    if (url.empty()) {
        MessageBoxW(g_mainHwnd, L"该程序未提供官方网站。", L"提示", MB_OK | MB_ICONINFORMATION);
        return;
    }
    ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void FillAppsList() {
    SendMessageW(g_appC.list, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(g_appC.list);
    for (size_t i = 0; i < g_apps.size(); ++i) {
        AppEntry& app = g_apps[i];
        // Persistent text: the list view retains the pointers.
        app.sizeText.clear();
        if (app.sizeKB) {
            wchar_t mb[64];
            swprintf_s(mb, L"%.1f MB", app.sizeKB / 1024.0);
            app.sizeText = mb;
        }

        LVITEMW item{};
        item.mask = LVIF_TEXT | LVIF_PARAM;
        item.iItem = (int)i;
        item.lParam = (LPARAM)i;
        item.pszText = (LPWSTR)app.name.c_str();
        int idx = ListView_InsertItem(g_appC.list, &item);
        if (idx < 0) continue;
        ListView_SetItemText(g_appC.list, idx, 1, (LPWSTR)app.version.c_str());
        ListView_SetItemText(g_appC.list, idx, 2, (LPWSTR)app.publisher.c_str());
        ListView_SetItemText(g_appC.list, idx, 3, (LPWSTR)app.sizeText.c_str());
        ListView_SetItemText(g_appC.list, idx, 4, (LPWSTR)app.installLocation.c_str());
    }
    SendMessageW(g_appC.list, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(g_appC.list, nullptr, TRUE);
    wchar_t buf[128];
    swprintf_s(buf, L"共 %zu 个程序", g_apps.size());
    SetWindowTextW(g_appC.status, buf);
}

void LoadSettingsToUi() {
    SendMessageW(g_setC.probe, BM_SETCHECK, g_settings.probeEnabled ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(g_setC.multiline, BM_SETCHECK, g_settings.onlyMultiline ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(g_setC.stop, BM_SETCHECK, g_settings.stopEverythingOnExit ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(g_setC.userOnly, BM_SETCHECK, g_settings.trimUserOnly ? BST_CHECKED : BST_UNCHECKED, 0);
    SetWindowTextW(g_setC.cap, std::to_wstring(g_settings.probeCap).c_str());
    SetWindowTextW(g_setC.maxmb, std::to_wstring(g_settings.probeMaxFileMB).c_str());
    SetWindowTextW(g_setC.resultcap, std::to_wstring(g_settings.resultCap).c_str());

    std::wstring ex;
    for (const auto& p : g_settings.excludePaths) {
        ex += p;
        ex += L"\r\n";
    }
    SetWindowTextW(g_setC.exclude, ex.c_str());
}

void SaveSettingsFromUi() {
    wchar_t buf[64];
    g_settings.probeEnabled = SendMessageW(g_setC.probe, BM_GETCHECK, 0, 0) == BST_CHECKED;
    g_settings.onlyMultiline = SendMessageW(g_setC.multiline, BM_GETCHECK, 0, 0) == BST_CHECKED;
    g_settings.stopEverythingOnExit = SendMessageW(g_setC.stop, BM_GETCHECK, 0, 0) == BST_CHECKED;
    g_settings.trimUserOnly = SendMessageW(g_setC.userOnly, BM_GETCHECK, 0, 0) == BST_CHECKED;

    GetWindowTextW(g_setC.cap, buf, 64);
    unsigned long v = wcstoul(buf, nullptr, 10);
    if (v) g_settings.probeCap = v;
    GetWindowTextW(g_setC.maxmb, buf, 64);
    v = wcstoul(buf, nullptr, 10);
    if (v) g_settings.probeMaxFileMB = v;
    GetWindowTextW(g_setC.resultcap, buf, 64);
    v = wcstoul(buf, nullptr, 10);
    if (v) g_settings.resultCap = v;

    g_settings.excludePaths.clear();
    std::wstring ex;
    int len = GetWindowTextLengthW(g_setC.exclude);
    if (len > 0) {
        ex.resize(len + 1);
        GetWindowTextW(g_setC.exclude, ex.data(), len + 1);
        ex.resize(len);
        size_t start = 0;
        while (start <= ex.size()) {
            size_t nl = ex.find_first_of(L"\r\n", start);
            std::wstring line = ex.substr(start, nl == std::wstring::npos ? std::wstring::npos : nl - start);
            while (!line.empty() && (line.back() == L' ' || line.back() == L'\t')) line.pop_back();
            if (!line.empty()) g_settings.excludePaths.push_back(line);
            if (nl == std::wstring::npos) break;
            start = nl;
            while (start < ex.size() && (ex[start] == L'\r' || ex[start] == L'\n')) ++start;
        }
    }
    SaveSettings(g_settings);
    MessageBoxW(g_mainHwnd, L"设置已保存。", L"设置", MB_OK | MB_ICONINFORMATION);
}

// ---------------- notifications ----------------

void OnGetDispInfo(NMLVDISPINFO* di, PageData& pd, int page) {
    static thread_local wchar_t textBuf[2048];
    textBuf[0] = 0;
    // Always provide a valid text pointer and no state by default: comctl32
    // crashes if pszText is left unset while the list is being resized.
    di->item.pszText = textBuf;
    di->item.stateMask = 0;

    std::lock_guard<std::mutex> lock(g_dataMutex);
    if (di->item.iItem < 0 || di->item.iItem >= (int)pd.order.size()) return;
    unsigned dataIdx = pd.order[di->item.iItem];
    if (dataIdx >= pd.items.size()) return;
    const FileItem& fi = pd.items[dataIdx];

    if (di->item.mask & LVIF_STATE) {
        if (dataIdx < pd.checked.size()) {
            di->item.stateMask = LVIS_STATEIMAGEMASK;
            di->item.state = INDEXTOSTATEIMAGEMASK(pd.checked[dataIdx] ? 2 : 1);
        }
    }
    if (di->item.mask & LVIF_TEXT) {
        switch (di->item.iSubItem) {
        case 0: break;
        case 1: swprintf_s(textBuf, L"%ls", FileNameFromPath(fi.path).c_str()); break;
        case 2: swprintf_s(textBuf, L"%ls", DirFromPath(fi.path).c_str()); break;
        case 3: swprintf_s(textBuf, L"%ls", FormatBytes(fi.size).c_str()); break;
        case 4:
            if (page == PAGE_IDLE)
                swprintf_s(textBuf, L"%ls", FormatFileTime(fi.dateRun).c_str());
            else
                swprintf_s(textBuf, L"%ls", FormatFileTime(fi.modified).c_str());
            break;
        case 5:
            if (page == PAGE_IDLE)
                swprintf_s(textBuf, L"%lu", fi.runCount);
            else
                swprintf_s(textBuf, L"%ls", fi.kind == 0 ? L"日志文件" :
                                  (fi.kind == 1 ? L"文本（探针）" : L"大文件"));
            break;
        }
    }
}

void OnClickCheckbox(HWND lv, NMITEMACTIVATE* nm, PageData& pd) {
    if (nm->iItem < 0) return;
    RECT rc{};
    if (!ListView_GetSubItemRect(lv, nm->iItem, 0, LVIR_BOUNDS, &rc)) return;
    if (nm->ptAction.x > rc.right) return;
    ToggleChecked(pd, lv, nm->iItem);
    RecalcSelected(pd);
    InvalidateRect(lv, nullptr, TRUE);
}

// ---------------- window proc ----------------

void OnCommand(int id, WPARAM wParam) {
    switch (id) {
    case IDC_LOG_REFRESH: StartScan(PAGE_LOG); break;
    case IDC_LARGE_REFRESH: StartScan(PAGE_LARGE); break;
    case IDC_LOG_SELALL: SetAllChecked(g_logPage, true); RecalcSelected(g_logPage); InvalidateRect(g_logC.list, nullptr, TRUE); UpdateStatusLine(PAGE_LOG); break;
    case IDC_LOG_SELNONE: SetAllChecked(g_logPage, false); RecalcSelected(g_logPage); InvalidateRect(g_logC.list, nullptr, TRUE); UpdateStatusLine(PAGE_LOG); break;
    case IDC_LOG_SELINV: InvertChecked(g_logPage); RecalcSelected(g_logPage); InvalidateRect(g_logC.list, nullptr, TRUE); UpdateStatusLine(PAGE_LOG); break;
    case IDC_LARGE_SELALL: SetAllChecked(g_largePage, true); RecalcSelected(g_largePage); InvalidateRect(g_largeC.list, nullptr, TRUE); UpdateStatusLine(PAGE_LARGE); break;
    case IDC_LARGE_SELNONE: SetAllChecked(g_largePage, false); RecalcSelected(g_largePage); InvalidateRect(g_largeC.list, nullptr, TRUE); UpdateStatusLine(PAGE_LARGE); break;
    case IDC_LARGE_SELINV: InvertChecked(g_largePage); RecalcSelected(g_largePage); InvalidateRect(g_largeC.list, nullptr, TRUE); UpdateStatusLine(PAGE_LARGE); break;
    case IDC_LOG_DELETE: StartDelete(PAGE_LOG); break;
    case IDC_LARGE_DELETE: StartDelete(PAGE_LARGE); break;
    case IDC_LOG_OPENFOLDER: OpenContainingFolder(GetSelectedPath(PAGE_LOG)); break;
    case IDC_LARGE_OPENFOLDER: OpenContainingFolder(GetSelectedPath(PAGE_LARGE)); break;
    case IDC_IDLE_REFRESH: StartScan(PAGE_IDLE); break;
    case IDC_IDLE_SELALL: SetAllChecked(g_idlePage, true); RecalcSelected(g_idlePage); InvalidateRect(g_idleC.list, nullptr, TRUE); UpdateStatusLine(PAGE_IDLE); break;
    case IDC_IDLE_SELNONE: SetAllChecked(g_idlePage, false); RecalcSelected(g_idlePage); InvalidateRect(g_idleC.list, nullptr, TRUE); UpdateStatusLine(PAGE_IDLE); break;
    case IDC_IDLE_SELINV: InvertChecked(g_idlePage); RecalcSelected(g_idlePage); InvalidateRect(g_idleC.list, nullptr, TRUE); UpdateStatusLine(PAGE_IDLE); break;
    case IDC_IDLE_DELETE: StartDelete(PAGE_IDLE); break;
    case IDC_IDLE_OPENFOLDER: OpenContainingFolder(GetSelectedPath(PAGE_IDLE)); break;

    case IDC_ARCHIVE_REFRESH: StartScan(PAGE_ARCHIVE); break;
    case IDC_ARCHIVE_SELALL: SetAllChecked(g_archivePage, true); RecalcSelected(g_archivePage); InvalidateRect(g_archiveC.list, nullptr, TRUE); UpdateStatusLine(PAGE_ARCHIVE); break;
    case IDC_ARCHIVE_SELNONE: SetAllChecked(g_archivePage, false); RecalcSelected(g_archivePage); InvalidateRect(g_archiveC.list, nullptr, TRUE); UpdateStatusLine(PAGE_ARCHIVE); break;
    case IDC_ARCHIVE_SELINV: InvertChecked(g_archivePage); RecalcSelected(g_archivePage); InvalidateRect(g_archiveC.list, nullptr, TRUE); UpdateStatusLine(PAGE_ARCHIVE); break;
    case IDC_ARCHIVE_DELETE: StartDelete(PAGE_ARCHIVE); break;
    case IDC_ARCHIVE_OPENFOLDER: OpenContainingFolder(GetSelectedPath(PAGE_ARCHIVE)); break;

    case IDC_INSTALLER_REFRESH: StartScan(PAGE_INSTALLER); break;
    case IDC_INSTALLER_SELALL: SetAllChecked(g_installerPage, true); RecalcSelected(g_installerPage); InvalidateRect(g_installerC.list, nullptr, TRUE); UpdateStatusLine(PAGE_INSTALLER); break;
    case IDC_INSTALLER_SELNONE: SetAllChecked(g_installerPage, false); RecalcSelected(g_installerPage); InvalidateRect(g_installerC.list, nullptr, TRUE); UpdateStatusLine(PAGE_INSTALLER); break;
    case IDC_INSTALLER_SELINV: InvertChecked(g_installerPage); RecalcSelected(g_installerPage); InvalidateRect(g_installerC.list, nullptr, TRUE); UpdateStatusLine(PAGE_INSTALLER); break;
    case IDC_INSTALLER_DELETE: StartDelete(PAGE_INSTALLER); break;
    case IDC_INSTALLER_OPENFOLDER: OpenContainingFolder(GetSelectedPath(PAGE_INSTALLER)); break;

    case IDC_MEM_REFRESH: RefreshMemory(); break;
    case IDC_MEM_PURGE: {
        auto* ctx = new ElevCtx();
        ctx->kind = 1;
        ctx->args = L"--purge-standby";
        std::thread(ElevWorker, ctx).detach();
        SetWindowTextW(g_memC.info, L"正在请求管理员权限清理待机列表…");
        break;
    }
    case IDC_MEM_TRIM: {
        SetCursor(LoadCursorW(nullptr, IDC_WAIT));
        size_t n = TrimWorkingSets(g_settings.trimUserOnly);
        SetCursor(LoadCursorW(nullptr, IDC_ARROW));
        wchar_t buf[128];
        swprintf_s(buf, L"已压缩 %zu 个进程的工作集。", n);
        MessageBoxW(g_mainHwnd, buf, L"压缩工作集", MB_OK | MB_ICONINFORMATION);
        break;
    }
    case IDC_MEM_END: EndSelectedProcess(); break;

    case IDC_SYSINFO_REFRESH:
        g_sysTempQueried = false;
        PopulateSysInfo();
        break;

    case IDC_APP_REFRESH: {
        SetWindowTextW(g_appC.status, L"正在读取已安装程序…");
        RefreshApps();
        break;
    }
    case IDC_APP_UNINSTALL: UninstallSelected(); break;
    case IDC_APP_OPENFOLDER: OpenAppFolder(); break;
    case IDC_APP_OPENURL: OpenAppUrl(); break;
    case IDC_APP_SYSSET:
        ShellExecuteW(nullptr, L"open", L"ms-settings:appsfeatures", nullptr, nullptr, SW_SHOWNORMAL);
        break;

    case IDC_SET_SAVE: SaveSettingsFromUi(); break;
    case IDC_SET_DEFAULT: {
        ResetSettings(g_settings);
        LoadSettingsToUi();
        SaveSettings(g_settings);
        break;
    }
    }

    if (HIWORD(wParam) == CBN_SELCHANGE && LOWORD(wParam) == IDC_LARGE_THRESHOLD) {
        int sel = (int)SendMessageW(g_largeC.threshold, CB_GETCURSEL, 0, 0);
        EnableWindow(g_largeC.custom, sel == 4);
    }
    if (HIWORD(wParam) == CBN_SELCHANGE && LOWORD(wParam) == IDC_IDLE_MODE) {
        int sel = (int)SendMessageW(g_idleC.mode, CB_GETCURSEL, 0, 0);
        EnableWindow(g_idleC.del, sel == 0);
    }
    if (HIWORD(wParam) == CBN_SELCHANGE && LOWORD(wParam) == IDC_IDLE_DAYS) {
        int sel = (int)SendMessageW(g_idleC.days, CB_GETCURSEL, 0, 0);
        EnableWindow(g_idleC.custom, sel == 4);
    }
}

void OnNotify(LPARAM lParam) {
    NMHDR* hdr = (NMHDR*)lParam;
    if (hdr->hwndFrom == g_tab && hdr->code == TCN_SELCHANGE) {
        ULONGLONG tSwitch = GetTickCount64();
        g_activeTab = (int)SendMessageW(g_tab, TCM_GETCURSEL, 0, 0);
        SetPageVisibility(g_activeTab);
        if (g_activeTab == PAGE_APPS && g_apps.empty()) {
            SetWindowTextW(g_appC.status, L"正在读取已安装程序…");
            RefreshApps();
        }
        if (g_activeTab == PAGE_MEM) {
            RefreshMemory();
            SetTimer(g_mainHwnd, 1, 2000, nullptr);
        } else {
            KillTimer(g_mainHwnd, 1);
        }
        if (g_activeTab == PAGE_SYSINFO) {
            if (!g_sysInfoReady) PopulateSysInfo();
            SetTimer(g_mainHwnd, 2, 1000, nullptr);
        } else {
            KillTimer(g_mainHwnd, 2);
        }
        RECT rc{};
        GetClientRect(g_mainHwnd, &rc);
        LayoutAll(rc.right, rc.bottom);
        DbgLog(L"tab switch to %d took %llu ms", g_activeTab, GetTickCount64() - tSwitch);
        return;
    }

    int id = (int)GetDlgCtrlID(hdr->hwndFrom);
    if (id == IDC_LOG_LIST || id == IDC_LARGE_LIST || id == IDC_IDLE_LIST ||
        id == IDC_ARCHIVE_LIST || id == IDC_INSTALLER_LIST) {
        PageData& pd = id == IDC_LOG_LIST ? g_logPage :
                       (id == IDC_LARGE_LIST ? g_largePage :
                       (id == IDC_IDLE_LIST ? g_idlePage :
                       (id == IDC_ARCHIVE_LIST ? g_archivePage : g_installerPage)));
        int page = id == IDC_LOG_LIST ? PAGE_LOG :
                   (id == IDC_LARGE_LIST ? PAGE_LARGE :
                   (id == IDC_IDLE_LIST ? PAGE_IDLE :
                   (id == IDC_ARCHIVE_LIST ? PAGE_ARCHIVE : PAGE_INSTALLER)));
        if (hdr->code == LVN_GETDISPINFO) {
            OnGetDispInfo((NMLVDISPINFO*)lParam, pd, page);
        } else if (hdr->code == LVN_COLUMNCLICK) {
            // Allow sorting even while background probing runs; the data is
            // consistent under the mutex and publish() rebuilds with the
            // user's chosen column.
            SortOnColumn(page, ((LPNMLISTVIEW)lParam)->iSubItem);
        } else if (hdr->code == NM_CLICK) {
            OnClickCheckbox(hdr->hwndFrom, (NMITEMACTIVATE*)lParam, pd);
            UpdateStatusLine(page);
        } else if (hdr->code == NM_DBLCLK) {
            OpenContainingFolder(GetSelectedPath(page));
        }
    } else if (id == IDC_MEM_LIST && hdr->code == NM_DBLCLK) {
        EndSelectedProcess();
    } else if (id == IDC_APP_LIST && hdr->code == NM_DBLCLK) {
        UninstallSelected();
    }
}

LRESULT CALLBACK MainWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        g_tab = CreateWindowExW(0, WC_TABCONTROLW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_TAB, g_hInst, nullptr);
        TCITEMW item{};
        item.mask = TCIF_TEXT;
        const wchar_t* tabs[] = {
            L"日志清理", L"大文件", L"久未使用",
            L"压缩包管理", L"安装包管理", L"内存清理",
            L"系统信息", L"软件卸载", L"设置"
        };
        for (int i = 0; i < 9; ++i) {
            item.pszText = (LPWSTR)tabs[i];
            SendMessageW(g_tab, TCM_INSERTITEM, i, (LPARAM)&item);
        }

        // log page
        g_logC.scope = MakeCombo(hwnd, IDC_LOG_SCOPE, 0, 0, 0, 0);
        SendMessageW(g_logC.scope, CB_ADDSTRING, 0, (LPARAM)L"全盘");
        SendMessageW(g_logC.scope, CB_ADDSTRING, 0, (LPARAM)L"用户目录");
        SendMessageW(g_logC.scope, CB_SETCURSEL, 0, 0);
        g_logC.time = MakeCombo(hwnd, IDC_LOG_TIME, 0, 0, 0, 0);
        SendMessageW(g_logC.time, CB_ADDSTRING, 0, (LPARAM)L"全部");
        SendMessageW(g_logC.time, CB_ADDSTRING, 0, (LPARAM)L"最近1天");
        SendMessageW(g_logC.time, CB_ADDSTRING, 0, (LPARAM)L"最近7天");
        SendMessageW(g_logC.time, CB_ADDSTRING, 0, (LPARAM)L"最近30天");
        SendMessageW(g_logC.time, CB_SETCURSEL, 0, 0);
        g_logC.multiline = MakeCheck(hwnd, IDC_LOG_MULTILINE, L"仅显示含换行的文本", 0, 0, 0, 0);
        g_logC.probe = MakeCheck(hwnd, IDC_LOG_PROBE, L"探测无后缀/未知后缀文件", 0, 0, 0, 0);
        g_logC.refresh = MakeButton(hwnd, IDC_LOG_REFRESH, L"刷新", 0, 0, 0, 0);
        g_logC.selAll = MakeButton(hwnd, IDC_LOG_SELALL, L"全选", 0, 0, 0, 0);
        g_logC.selNone = MakeButton(hwnd, IDC_LOG_SELNONE, L"全不选", 0, 0, 0, 0);
        g_logC.selInv = MakeButton(hwnd, IDC_LOG_SELINV, L"反选", 0, 0, 0, 0);
        g_logC.del = MakeButton(hwnd, IDC_LOG_DELETE, L"删除到回收站", 0, 0, 0, 0);
        g_logC.openFolder = MakeButton(hwnd, IDC_LOG_OPENFOLDER, L"打开所在文件夹", 0, 0, 0, 0);
        const wchar_t* logCols[] = { L"", L"名称", L"位置", L"大小", L"修改时间", L"类型" };
        const int logW[] = { 40, 240, 320, 90, 140, 110 };
        g_logC.list = MakeList(hwnd, IDC_LOG_LIST, true, 0, 0, 0, 0);
        AddColumns(g_logC.list, logCols, logW, 6);
        g_logC.status = MakeStatic(hwnd, IDC_LOG_STATUS, L"点击“刷新”开始扫描日志文件", 0, 0, 0, 0);
        g_logC.progress = CreateWindowExW(0, PROGRESS_CLASSW, L"", WS_CHILD | PBS_SMOOTH,
                                          0, 0, 0, 0, hwnd, nullptr, g_hInst, nullptr);
        ShowWindow(g_logC.progress, SW_HIDE);

        // large page
        g_largeC.threshold = MakeCombo(hwnd, IDC_LARGE_THRESHOLD, 0, 0, 0, 0);
        const wchar_t* sizes[] = { L"50 MB", L"100 MB", L"500 MB", L"1 GB", L"自定义" };
        for (auto s : sizes) SendMessageW(g_largeC.threshold, CB_ADDSTRING, 0, (LPARAM)s);
        SendMessageW(g_largeC.threshold, CB_SETCURSEL, 1, 0);
        g_largeC.custom = MakeEdit(hwnd, IDC_LARGE_CUSTOM, 0, 0, 0, 0);
        SetWindowTextW(g_largeC.custom, L"");
        g_largeC.refresh = MakeButton(hwnd, IDC_LARGE_REFRESH, L"刷新", 0, 0, 0, 0);
        g_largeC.selAll = MakeButton(hwnd, IDC_LARGE_SELALL, L"全选", 0, 0, 0, 0);
        g_largeC.selNone = MakeButton(hwnd, IDC_LARGE_SELNONE, L"全不选", 0, 0, 0, 0);
        g_largeC.selInv = MakeButton(hwnd, IDC_LARGE_SELINV, L"反选", 0, 0, 0, 0);
        g_largeC.del = MakeButton(hwnd, IDC_LARGE_DELETE, L"删除到回收站", 0, 0, 0, 0);
        g_largeC.openFolder = MakeButton(hwnd, IDC_LARGE_OPENFOLDER, L"打开所在文件夹", 0, 0, 0, 0);
        const wchar_t* largeCols[] = { L"", L"名称", L"位置", L"大小", L"修改时间" };
        const int largeW[] = { 40, 240, 360, 100, 140 };
        g_largeC.list = MakeList(hwnd, IDC_LARGE_LIST, true, 0, 0, 0, 0);
        AddColumns(g_largeC.list, largeCols, largeW, 5);
        g_largeC.status = MakeStatic(hwnd, IDC_LARGE_STATUS, L"点击“刷新”开始查找大文件", 0, 0, 0, 0);
        g_largeC.progress = CreateWindowExW(0, PROGRESS_CLASSW, L"", WS_CHILD | PBS_SMOOTH,
                                            0, 0, 0, 0, hwnd, nullptr, g_hInst, nullptr);
        ShowWindow(g_largeC.progress, SW_HIDE);

        // archive page
        g_archiveC.refresh = MakeButton(hwnd, IDC_ARCHIVE_REFRESH, L"刷新", 0, 0, 0, 0);
        g_archiveC.selAll = MakeButton(hwnd, IDC_ARCHIVE_SELALL, L"全选", 0, 0, 0, 0);
        g_archiveC.selNone = MakeButton(hwnd, IDC_ARCHIVE_SELNONE, L"全不选", 0, 0, 0, 0);
        g_archiveC.selInv = MakeButton(hwnd, IDC_ARCHIVE_SELINV, L"反选", 0, 0, 0, 0);
        g_archiveC.del = MakeButton(hwnd, IDC_ARCHIVE_DELETE, L"删除到回收站", 0, 0, 0, 0);
        g_archiveC.openFolder = MakeButton(hwnd, IDC_ARCHIVE_OPENFOLDER, L"打开所在文件夹", 0, 0, 0, 0);
        const wchar_t* archiveCols[] = { L"", L"名称", L"位置", L"大小", L"修改时间" };
        const int archiveW[] = { 40, 240, 360, 100, 140 };
        g_archiveC.list = MakeList(hwnd, IDC_ARCHIVE_LIST, true, 0, 0, 0, 0);
        AddColumns(g_archiveC.list, archiveCols, archiveW, 5);
        g_archiveC.status = MakeStatic(hwnd, IDC_ARCHIVE_STATUS, L"点击“刷新”开始查找压缩包", 0, 0, 0, 0);
        g_archiveC.progress = CreateWindowExW(0, PROGRESS_CLASSW, L"", WS_CHILD | PBS_SMOOTH,
                                              0, 0, 0, 0, hwnd, nullptr, g_hInst, nullptr);
        ShowWindow(g_archiveC.progress, SW_HIDE);

        // installer page
        g_installerC.refresh = MakeButton(hwnd, IDC_INSTALLER_REFRESH, L"刷新", 0, 0, 0, 0);
        g_installerC.selAll = MakeButton(hwnd, IDC_INSTALLER_SELALL, L"全选", 0, 0, 0, 0);
        g_installerC.selNone = MakeButton(hwnd, IDC_INSTALLER_SELNONE, L"全不选", 0, 0, 0, 0);
        g_installerC.selInv = MakeButton(hwnd, IDC_INSTALLER_SELINV, L"反选", 0, 0, 0, 0);
        g_installerC.del = MakeButton(hwnd, IDC_INSTALLER_DELETE, L"删除到回收站", 0, 0, 0, 0);
        g_installerC.openFolder = MakeButton(hwnd, IDC_INSTALLER_OPENFOLDER, L"打开所在文件夹", 0, 0, 0, 0);
        const wchar_t* installerCols[] = { L"", L"名称", L"位置", L"大小", L"修改时间" };
        const int installerW[] = { 40, 240, 360, 100, 140 };
        g_installerC.list = MakeList(hwnd, IDC_INSTALLER_LIST, true, 0, 0, 0, 0);
        AddColumns(g_installerC.list, installerCols, installerW, 5);
        g_installerC.status = MakeStatic(hwnd, IDC_INSTALLER_STATUS, L"点击“刷新”开始查找安装包", 0, 0, 0, 0);
        g_installerC.progress = CreateWindowExW(0, PROGRESS_CLASSW, L"", WS_CHILD | PBS_SMOOTH,
                                                0, 0, 0, 0, hwnd, nullptr, g_hInst, nullptr);
        ShowWindow(g_installerC.progress, SW_HIDE);

        // system info page
        g_sysC.refresh = MakeButton(hwnd, IDC_SYSINFO_REFRESH, L"刷新", 0, 0, 0, 0);
        const wchar_t* sysCols[] = { L"项目", L"值" };
        const int sysW[] = { 180, 520 };
        g_sysC.list = MakeList(hwnd, IDC_SYSINFO_LIST, false, 0, 0, 0, 0);
        AddColumns(g_sysC.list, sysCols, sysW, 2);

        // idle (long-unused) page
        g_idleC.mode = MakeCombo(hwnd, IDC_IDLE_MODE, 0, 0, 0, 0);
        SendMessageW(g_idleC.mode, CB_ADDSTRING, 0, (LPARAM)L"文件");
        SendMessageW(g_idleC.mode, CB_ADDSTRING, 0, (LPARAM)L"程序（exe 等）");
        SendMessageW(g_idleC.mode, CB_SETCURSEL, 0, 0);
        g_idleC.days = MakeCombo(hwnd, IDC_IDLE_DAYS, 0, 0, 0, 0);
        SendMessageW(g_idleC.days, CB_ADDSTRING, 0, (LPARAM)L"30 天");
        SendMessageW(g_idleC.days, CB_ADDSTRING, 0, (LPARAM)L"90 天");
        SendMessageW(g_idleC.days, CB_ADDSTRING, 0, (LPARAM)L"180 天");
        SendMessageW(g_idleC.days, CB_ADDSTRING, 0, (LPARAM)L"365 天");
        SendMessageW(g_idleC.days, CB_ADDSTRING, 0, (LPARAM)L"自定义（天）");
        SendMessageW(g_idleC.days, CB_SETCURSEL, 1, 0);
        g_idleC.custom = MakeEdit(hwnd, IDC_IDLE_CUSTOM, 0, 0, 0, 0);
        SetWindowTextW(g_idleC.custom, L"");
        g_idleC.refresh = MakeButton(hwnd, IDC_IDLE_REFRESH, L"刷新", 0, 0, 0, 0);
        g_idleC.selAll = MakeButton(hwnd, IDC_IDLE_SELALL, L"全选", 0, 0, 0, 0);
        g_idleC.selNone = MakeButton(hwnd, IDC_IDLE_SELNONE, L"全不选", 0, 0, 0, 0);
        g_idleC.selInv = MakeButton(hwnd, IDC_IDLE_SELINV, L"反选", 0, 0, 0, 0);
        g_idleC.del = MakeButton(hwnd, IDC_IDLE_DELETE, L"删除到回收站", 0, 0, 0, 0);
        g_idleC.openFolder = MakeButton(hwnd, IDC_IDLE_OPENFOLDER, L"打开所在文件夹", 0, 0, 0, 0);
        const wchar_t* idleCols[] = { L"", L"名称", L"位置", L"大小", L"最后使用", L"使用次数" };
        const int idleW[] = { 40, 240, 340, 90, 140, 80 };
        g_idleC.list = MakeList(hwnd, IDC_IDLE_LIST, true, 0, 0, 0, 0);
        AddColumns(g_idleC.list, idleCols, idleW, 6);
        g_idleC.status = MakeStatic(hwnd, IDC_IDLE_STATUS,
                                    L"查找很久未打开/使用的文件（基于 Everything 运行历史）", 0, 0, 0, 0);
        g_idleC.progress = CreateWindowExW(0, PROGRESS_CLASSW, L"", WS_CHILD | PBS_SMOOTH,
                                           0, 0, 0, 0, hwnd, nullptr, g_hInst, nullptr);
        ShowWindow(g_idleC.progress, SW_HIDE);

        // memory page
        g_memC.info = MakeStatic(hwnd, IDC_MEM_INFO, L"正在读取内存信息…", 0, 0, 0, 0);
        g_memC.purge = MakeButton(hwnd, IDC_MEM_PURGE, L"清理待机列表（需管理员）", 0, 0, 0, 0);
        g_memC.trim = MakeButton(hwnd, IDC_MEM_TRIM, L"压缩工作集", 0, 0, 0, 0);
        g_memC.refresh = MakeButton(hwnd, IDC_MEM_REFRESH, L"刷新", 0, 0, 0, 0);
        g_memC.userOnly = MakeCheck(hwnd, IDC_MEM_USERONLY, L"仅压缩当前用户进程", 0, 0, 0, 0);
        g_memC.end = MakeButton(hwnd, IDC_MEM_END, L"结束进程", 0, 0, 0, 0);
        const wchar_t* memCols[] = { L"进程", L"PID", L"工作集", L"私有内存" };
        const int memW[] = { 240, 90, 120, 120 };
        g_memC.list = MakeList(hwnd, IDC_MEM_LIST, false, 0, 0, 0, 0);
        AddColumns(g_memC.list, memCols, memW, 4);

        // apps page
        g_appC.uninstall = MakeButton(hwnd, IDC_APP_UNINSTALL, L"卸载", 0, 0, 0, 0);
        g_appC.openFolder = MakeButton(hwnd, IDC_APP_OPENFOLDER, L"打开安装目录", 0, 0, 0, 0);
        g_appC.openUrl = MakeButton(hwnd, IDC_APP_OPENURL, L"官方网站", 0, 0, 0, 0);
        g_appC.refresh = MakeButton(hwnd, IDC_APP_REFRESH, L"刷新", 0, 0, 0, 0);
        g_appC.sysset = MakeButton(hwnd, IDC_APP_SYSSET, L"系统应用和功能", 0, 0, 0, 0);
        const wchar_t* appCols[] = { L"名称", L"版本", L"发布者", L"大小", L"安装位置" };
        const int appW[] = { 250, 110, 150, 80, 300 };
        g_appC.list = MakeList(hwnd, IDC_APP_LIST, false, 0, 0, 0, 0);
        AddColumns(g_appC.list, appCols, appW, 5);
        g_appC.status = MakeStatic(hwnd, IDC_APP_STATUS, L"点击“刷新”读取已安装程序", 0, 0, 0, 0);

        // settings page
        g_setC.probe = MakeCheck(hwnd, IDC_SET_PROBE, L"启用未知格式文本探针", 0, 0, 0, 0);
        g_setC.multiline = MakeCheck(hwnd, IDC_SET_MULTILINE, L"仅显示含换行的文本", 0, 0, 0, 0);
        g_setC.stop = MakeCheck(hwnd, IDC_SET_STOP, L"退出时停止 Everything", 0, 0, 0, 0);
        g_setC.userOnly = MakeCheck(hwnd, IDC_SET_USERONLY, L"仅压缩当前用户进程", 0, 0, 0, 0);
        g_setC.cap = MakeEdit(hwnd, IDC_SET_CAP, 0, 0, 0, 0);
        g_setC.maxmb = MakeEdit(hwnd, IDC_SET_MAXMB, 0, 0, 0, 0);
        g_setC.resultcap = MakeEdit(hwnd, IDC_SET_RESULTCAP, 0, 0, 0, 0);
        g_setC.lblCap = MakeStatic(hwnd, IDC_SET_LBL_CAP, L"探测文件上限", 0, 0, 0, 0);
        g_setC.lblMaxmb = MakeStatic(hwnd, IDC_SET_LBL_MAXMB, L"最大文件MB", 0, 0, 0, 0);
        g_setC.lblResult = MakeStatic(hwnd, IDC_SET_LBL_RESULT, L"结果上限", 0, 0, 0, 0);
        g_setC.lblExclude = MakeStatic(hwnd, IDC_SET_LBL_EXCLUDE,
                                       L"排除路径（每行一个，不区分大小写）：", 0, 0, 0, 0);
        g_setC.exclude = MakeEdit(hwnd, IDC_SET_EXCLUDE, 0, 0, 0, 0, ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL);
        g_setC.save = MakeButton(hwnd, IDC_SET_SAVE, L"保存设置", 0, 0, 0, 0);
        g_setC.reset = MakeButton(hwnd, IDC_SET_DEFAULT, L"恢复默认", 0, 0, 0, 0);
        g_setC.about = MakeStatic(hwnd, IDC_SET_ABOUT,
            L"WindowsClean v1.0（C++/Win32，性能优先）\r\n"
            L"基于 Everything SDK 实现秒级文件检索。\r\n"
            L"\r\n"
            L"Everything Copyright (C) 2008-2026 voidtools，基于 MIT 许可分发。\r\n"
            L"Everything SDK 与 Everything 本体均为 MIT 许可，允许免费再分发。", 0, 0, 0, 0);

        HWND all[] = {
            g_logC.scope, g_logC.time, g_logC.multiline, g_logC.probe, g_logC.refresh,
            g_logC.selAll, g_logC.selNone, g_logC.selInv, g_logC.del, g_logC.openFolder,
            g_logC.list, g_logC.status, g_logC.progress,
            g_largeC.threshold, g_largeC.custom, g_largeC.refresh, g_largeC.selAll,
            g_largeC.selNone, g_largeC.selInv, g_largeC.del, g_largeC.openFolder,
            g_largeC.list, g_largeC.status, g_largeC.progress,
            g_archiveC.refresh, g_archiveC.selAll, g_archiveC.selNone, g_archiveC.selInv,
            g_archiveC.del, g_archiveC.openFolder, g_archiveC.list, g_archiveC.status,
            g_archiveC.progress,
            g_installerC.refresh, g_installerC.selAll, g_installerC.selNone, g_installerC.selInv,
            g_installerC.del, g_installerC.openFolder, g_installerC.list, g_installerC.status,
            g_installerC.progress,
            g_memC.info, g_memC.purge, g_memC.trim, g_memC.refresh, g_memC.userOnly,
            g_memC.list, g_memC.end,
            g_sysC.refresh, g_sysC.list,
            g_idleC.mode, g_idleC.days, g_idleC.custom, g_idleC.refresh,
            g_idleC.selAll, g_idleC.selNone, g_idleC.selInv, g_idleC.del,
            g_idleC.openFolder, g_idleC.list, g_idleC.status, g_idleC.progress,
            g_appC.uninstall, g_appC.openFolder, g_appC.openUrl, g_appC.refresh,
            g_appC.sysset, g_appC.list, g_appC.status,
            g_setC.probe, g_setC.multiline, g_setC.stop, g_setC.userOnly, g_setC.lblCap,
            g_setC.cap, g_setC.lblMaxmb, g_setC.maxmb, g_setC.lblResult, g_setC.resultcap,
            g_setC.lblExclude, g_setC.exclude, g_setC.save, g_setC.reset, g_setC.about
        };
        for (HWND c : all) SetDlgFont(c);

        SetPageVisibility(0);

        g_settings = Settings();
        LoadSettings(g_settings);
        SendMessageW(g_logC.probe, BM_SETCHECK, g_settings.probeEnabled ? BST_CHECKED : BST_UNCHECKED, 0);
        SendMessageW(g_logC.multiline, BM_SETCHECK, g_settings.onlyMultiline ? BST_CHECKED : BST_UNCHECKED, 0);
        SendMessageW(g_memC.userOnly, BM_SETCHECK, g_settings.trimUserOnly ? BST_CHECKED : BST_UNCHECKED, 0);
        LoadSettingsToUi();
        return 0;
    }

    case WM_SIZE:
        LayoutAll(LOWORD(lParam), HIWORD(lParam));
        return 0;

    case WM_GETMINMAXINFO: {
        MINMAXINFO* mmi = (MINMAXINFO*)lParam;
        mmi->ptMinTrackSize.x = Scale(hwnd, 860);
        mmi->ptMinTrackSize.y = Scale(hwnd, 560);
        return 0;
    }

    case WM_COMMAND:
        OnCommand(LOWORD(wParam), wParam);
        return 0;

    case WM_NOTIFY:
        OnNotify(lParam);
        return 0;

    case WM_TIMER:
        if (wParam == 1 && g_activeTab == PAGE_MEM) RefreshMemInfoOnly();
        else if (wParam == 2 && g_activeTab == PAGE_SYSINFO) RefreshLiveRows();
        return 0;

    case WM_APP_SCAN_PROGRESS: {
        uint64_t packed = (uint64_t)lParam;
        size_t done = (size_t)(packed & 0xFFFFFFFF);
        size_t total = (size_t)(packed >> 32);
        SetProgress(0, done, total);
        wchar_t buf[128];
        swprintf_s(buf, L"正在探测文件类型… %zu / %zu", done, total);
        SetStatusText(g_logC.status, buf);
        return 0;
    }

    case WM_APP_SCAN_DONE: {
        ScanDoneMsg* msg = (ScanDoneMsg*)lParam;
        PageData& pd = PageFor(msg->page);
        if (msg->error) {
            pd.scanning = false;
            g_scanRunning = false;
            SetBusy(msg->page, false);
            ShowScanError(msg->page, msg->error);
        } else {
            if (msg->done) {
                pd.scanning = false;
                g_scanRunning = false;
                SetBusy(msg->page, false);
            }
            UpdateListCount(msg->page);
            RecalcSelected(PageFor(msg->page));
            wchar_t buf[640];
            if (!msg->done) {
                swprintf_s(buf, L"已显示 %zu 项已知日志 · 探测 %zu · 不可读 %zu · 耗时 %llu ms · 正在后台探测无扩展名/未知后缀文件…",
                           msg->total, msg->probed, msg->unreadable,
                           (unsigned long long)msg->elapsedMs);
            } else if (msg->truncated) {
                swprintf_s(buf, L"共 %zu 项 · 已勾选 %llu · 合计 %s · 探测 %zu · 不可读 %zu · 耗时 %llu ms（部分结果达到上限，可能不完整）",
                           msg->total, (unsigned long long)pd.selCount,
                           FormatBytes(pd.selBytes).c_str(),
                           msg->probed, msg->unreadable,
                           (unsigned long long)msg->elapsedMs);
            } else {
                swprintf_s(buf, L"共 %zu 项 · 已勾选 %llu · 合计 %s · 探测 %zu · 不可读 %zu · 耗时 %llu ms",
                           msg->total, (unsigned long long)pd.selCount,
                           FormatBytes(pd.selBytes).c_str(),
                           msg->probed, msg->unreadable,
                           (unsigned long long)msg->elapsedMs);
            }
            SetStatusText(StatusFor(msg->page), buf);
        }
        delete msg;
        return 0;
    }

    case WM_APP_DELETE_PROGRESS: {
        uint64_t packed = (uint64_t)lParam;
        size_t done = (size_t)(packed & 0xFFFFFFFF);
        size_t total = (size_t)(packed >> 32);
        SetProgress((int)wParam, done, total);
        wchar_t buf[128];
        swprintf_s(buf, L"正在删除到回收站… %zu / %zu", done, total);
        SetStatusText(StatusFor((int)wParam), buf);
        return 0;
    }

    case WM_APP_DELETE_DONE: {
        int page = (int)wParam;
        DeleteStats* stats = (DeleteStats*)lParam;
        PageData& pd = PageFor(page);
        {
            std::lock_guard<std::mutex> lock(g_dataMutex);
            std::unordered_set<std::wstring> failed(stats->failedPaths.begin(), stats->failedPaths.end());
            std::unordered_set<std::wstring> submitted(stats->submitted.begin(), stats->submitted.end());
            FileVec newItems;
            newItems.reserve(pd.items.size());
            std::vector<unsigned char> newChecked;
            newChecked.reserve(pd.checked.size());
            for (size_t i = 0; i < pd.items.size(); ++i) {
                bool inSubmitted = submitted.count(pd.items[i].path) != 0;
                bool isFailed = failed.count(pd.items[i].path) != 0;
                if (inSubmitted && !isFailed) continue; // deleted successfully
                newItems.push_back(std::move(pd.items[i]));
                newChecked.push_back(pd.checked[i]);
            }
            pd.items = std::move(newItems);
            pd.checked = std::move(newChecked);
            BuildSortedOrder(pd.items, pd.order, pd.sortCol, pd.sortAsc, page == PAGE_IDLE);
        }
        SetBusy(page, false);
        UpdateListCount(page);
        RecalcSelected(pd);
        UpdateStatusLine(page);

        wchar_t buf[256];
        swprintf_s(buf, L"已删除 %zu 项，失败 %zu 项%s。", stats->ok, stats->failed,
                   stats->permanent ? L"（含网络盘永久删除）" : L"");
        SetStatusText(StatusFor(page), buf);

        if (!stats->failedPaths.empty()) {
            if (MessageBoxW(g_mainHwnd, L"有文件未能删除（通常是没有权限）。是否以管理员身份重试删除这些文件？",
                            L"删除", MB_YESNO | MB_ICONWARNING) == IDYES) {
                auto* ctx = new ElevCtx();
                ctx->kind = 0;
                ctx->page = page;
                ctx->failedPaths = stats->failedPaths;
                std::wstring listFile = GetAppDataDir() + L"\\retry_paths.bin";
                std::wstring resultFile = GetAppDataDir() + L"\\retry_result.txt";
                WritePathListFile(listFile, stats->failedPaths);
                ctx->args = L"--delete-list \"" + listFile + L"\" --result \"" + resultFile + L"\"";
                ctx->resultFile = resultFile;
                std::thread(ElevWorker, ctx).detach();
            }
        }
        delete stats;
        return 0;
    }

    case WM_APP_ELEVATED_DONE: {
        ElevCtx* ctx = (ElevCtx*)lParam;
        DWORD code = (DWORD)wParam;
        if (ctx->kind == 1) {
            RefreshMemory();
            MessageBoxW(g_mainHwnd,
                        code == 0 ? L"待机列表清理完成。" : L"待机列表清理失败（需要管理员权限）。",
                        L"内存清理", MB_OK | MB_ICONINFORMATION);
        } else if (ctx->kind == 2) {
            RefreshMemory();
            MessageBoxW(g_mainHwnd,
                        code == 0 ? L"已以管理员身份结束进程。" : L"以管理员身份结束进程失败。",
                        L"结束进程", MB_OK | MB_ICONINFORMATION);
        } else if (ctx->kind == 0) {
            std::wstring detail = ctx->resultText.empty() ? L"" : (L"\n" + ctx->resultText);
            // Compact the list in place: drop retried paths that are now gone
            // (no full re-scan, no re-probing).
            PageData& pd = PageFor(ctx->page);
            size_t removed = 0;
            size_t stillThere = 0;
            std::unordered_set<std::wstring> retried(ctx->failedPaths.begin(),
                                                     ctx->failedPaths.end());
            {
                std::lock_guard<std::mutex> lock(g_dataMutex);
                FileVec newItems;
                newItems.reserve(pd.items.size());
                std::vector<unsigned char> newChecked;
                newChecked.reserve(pd.checked.size());
                for (size_t i = 0; i < pd.items.size(); ++i) {
                    if (retried.count(pd.items[i].path)) {
                        if (GetFileAttributesW(pd.items[i].path.c_str()) == INVALID_FILE_ATTRIBUTES) {
                            ++removed;
                            continue; // deleted by the elevated retry
                        }
                        ++stillThere;
                    }
                    newItems.push_back(std::move(pd.items[i]));
                    newChecked.push_back(pd.checked[i]);
                }
                pd.items = std::move(newItems);
                pd.checked = std::move(newChecked);
                BuildSortedOrder(pd.items, pd.order, pd.sortCol, pd.sortAsc,
                                 ctx->page == PAGE_IDLE);
            }
            UpdateListCount(ctx->page);
            RecalcSelected(pd);
            UpdateStatusLine(ctx->page);

            wchar_t buf[320];
            swprintf_s(buf, L"管理员删除完成：已删除 %zu 项，仍有 %zu 项（可能被占用或需手动处理）。",
                       removed, stillThere);
            SetStatusText(StatusFor(ctx->page), buf);
            MessageBoxW(g_mainHwnd, (std::wstring(buf) + detail).c_str(),
                        L"删除", MB_OK | MB_ICONINFORMATION);

            // clean up the temporary path list
            std::wstring listFile = GetAppDataDir() + L"\\retry_paths.bin";
            DeleteFileW(listFile.c_str());
        }
        delete ctx;
        return 0;
    }

    case WM_APP_APPS_DONE: {
        AppsDoneMsg* msg = (AppsDoneMsg*)lParam;
        // Release the old rows' text pointers before the backing strings die.
        ListView_DeleteAllItems(g_appC.list);
        g_apps = std::move(msg->apps);
        delete msg;
        FillAppsList();
        return 0;
    }

    case WM_APP_PROC_DONE: {
        std::vector<ProcEntry>* procs = (std::vector<ProcEntry>*)lParam;
        // Release the old rows' text pointers before the backing strings die.
        ListView_DeleteAllItems(g_memC.list);
        g_procs = std::move(*procs);
        delete procs;
        FillProcList(g_procs);
        g_procRefreshRunning = false;
        if (g_procRefreshPending) {
            g_procRefreshPending = false;
            RefreshMemory();
        }
        return 0;
    }

    case WM_APP_SYSINFO_TEMP_DONE: {
        TempResult* tr = (TempResult*)lParam;
        SetTempRow(tr->ok, tr->celsius);
        delete tr;
        return 0;
    }

    case WM_DPICHANGED: {
        RECT* rc = (RECT*)lParam;
        SetWindowPos(hwnd, nullptr, rc->left, rc->top, rc->right - rc->left, rc->bottom - rc->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }

    case WM_CLOSE:
        DbgLog(L"WM_CLOSE");
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        DbgLog(L"WM_DESTROY");
        KillTimer(hwnd, 1);
        KillTimer(hwnd, 2);
        ShutdownEngine(g_settings.stopEverythingOnExit && EngineStartedByUs());
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace

bool RegisterMainWindowClass(HINSTANCE hInst) {
    g_hInst = hInst;
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = MainWndProc;
    wc.hInstance = hInst;
    wc.hIcon = LoadIconW(hInst, MAKEINTRESOURCEW(IDI_APP));
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"WindowsCleanMainWnd";
    return RegisterClassExW(&wc) != 0;
}

HWND CreateMainWindow(HINSTANCE hInst) {
    g_uiFont = CreateFontW(-MulDiv(9, GetDpiForSystem(), 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                           DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                           DEFAULT_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
    CreateStateImages();

    int sx = GetSystemMetrics(SM_CXSCREEN);
    int sy = GetSystemMetrics(SM_CYSCREEN);
    int w = 1080, h = 700;
    int x = std::max(0, (sx - w) / 2);
    int y = std::max(0, (sy - h) / 2);

    g_mainHwnd = CreateWindowExW(0, L"WindowsCleanMainWnd", L"WindowsClean - 轻量电脑清理",
                                 WS_OVERLAPPEDWINDOW, x, y, w, h,
                                 nullptr, nullptr, hInst, nullptr);
    if (g_mainHwnd) {
        SendMessageW(g_mainHwnd, WM_SETICON, ICON_BIG, (LPARAM)LoadIconW(hInst, MAKEINTRESOURCEW(IDI_APP)));
        ShowWindow(g_mainHwnd, SW_SHOW);
        UpdateWindow(g_mainHwnd);
    }
    return g_mainHwnd;
}
