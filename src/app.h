#pragma once

#include "config.h"
#include "filelist.h"
#include "resource.h"
#include "util.h"

#include <mutex>
#include <vector>

#define WM_APP_SCAN_DONE       (WM_APP + 1)
#define WM_APP_SCAN_PROGRESS   (WM_APP + 2)
#define WM_APP_DELETE_PROGRESS (WM_APP + 3)
#define WM_APP_DELETE_DONE     (WM_APP + 4)
#define WM_APP_ELEVATED_DONE   (WM_APP + 5)
#define WM_APP_APPS_DONE       (WM_APP + 6)
#define WM_APP_PROC_DONE       (WM_APP + 7)
#define WM_APP_SYSINFO_TEMP_DONE (WM_APP + 8)

enum PageKind {
    PAGE_LOG = 0,
    PAGE_LARGE = 1,
    PAGE_IDLE = 2,
    PAGE_ARCHIVE = 3,
    PAGE_INSTALLER = 4,
    PAGE_MEM = 5,
    PAGE_SYSINFO = 6,
    PAGE_APPS = 7,
    PAGE_SET = 8,
};

struct PageData {
    FileVec items;
    std::vector<unsigned> order;
    std::vector<unsigned char> checked;
    int sortCol = 0;
    bool sortAsc = true;
    bool scanning = false;
    uint64_t selCount = 0;
    uint64_t selBytes = 0;
};

extern Settings g_settings;
extern HWND g_mainHwnd;
extern PageData g_logPage;
extern PageData g_largePage;
extern PageData g_archivePage;
extern PageData g_installerPage;
extern std::mutex g_dataMutex;

bool RegisterMainWindowClass(HINSTANCE hInst);
HWND CreateMainWindow(HINSTANCE hInst);
