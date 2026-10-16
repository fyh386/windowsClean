#include "app.h"

#include "filelist.h"
#include "memclean.h"
#include "scanner.h"
#include "sysinfo.h"
#include "trash.h"
#include "uninstall.h"
#include "util.h"

#include <gtk/gtk.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <thread>
#include <utility>

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

namespace {

struct Config {
    int scope = 0;            // 0 whole disk, 1 home
    int logTimeDays = 0;      // 0/1/7/30
    bool probe = true;
    bool onlyMultiline = true;
    int idleDays = 90;
    int idleMode = 0;
    uint64_t minSizeMB = 100;
    size_t resultCap = 200000;
    std::vector<std::string> excludePaths;
};

Config g_cfg;
std::string g_configPath;

std::string HomeDir() {
    const char* h = getenv("HOME");
    return h ? h : "/root";
}

std::string ConfigDir() {
    const char* xdg = getenv("XDG_CONFIG_HOME");
    std::string dir = (xdg && *xdg) ? xdg : (HomeDir() + "/.config");
    return dir + "/ubuntu-cleaner";
}

std::string ConfigPath() {
    return ConfigDir() + "/config.ini";
}

void LoadConfig() {
    g_configPath = ConfigPath();
    auto gv = [](const char* k) { return GetIniValue(g_configPath, k); };
    auto iv = [&](const char* k, int def) {
        std::string v = gv(k);
        return v.empty() ? def : atoi(v.c_str());
    };
    g_cfg.scope = iv("scope", 0);
    g_cfg.logTimeDays = iv("logTimeDays", 0);
    g_cfg.probe = iv("probe", 1) != 0;
    g_cfg.onlyMultiline = iv("onlyMultiline", 1) != 0;
    g_cfg.idleDays = iv("idleDays", 90);
    g_cfg.idleMode = iv("idleMode", 0);
    g_cfg.minSizeMB = (uint64_t)iv("minSizeMB", 100);
    g_cfg.resultCap = (size_t)iv("resultCap", 200000);

    g_cfg.excludePaths.clear();
    std::string ex = gv("exclude");
    size_t start = 0;
    while (start <= ex.size()) {
        size_t sep = ex.find(';', start);
        std::string p = Trim(ex.substr(start, sep == std::string::npos ? std::string::npos : sep - start));
        if (!p.empty()) g_cfg.excludePaths.push_back(p);
        if (sep == std::string::npos) break;
        start = sep + 1;
    }
}

void SaveConfig() {
    g_configPath = ConfigPath();
    std::string dir = ConfigDir();
    std::string cmd = "mkdir -p '" + dir + "'";
    if (system(cmd.c_str()) != 0) { /* ignore */ }
    SetIniValue(g_configPath, "scope", std::to_string(g_cfg.scope));
    SetIniValue(g_configPath, "logTimeDays", std::to_string(g_cfg.logTimeDays));
    SetIniValue(g_configPath, "probe", g_cfg.probe ? "1" : "0");
    SetIniValue(g_configPath, "onlyMultiline", g_cfg.onlyMultiline ? "1" : "0");
    SetIniValue(g_configPath, "idleDays", std::to_string(g_cfg.idleDays));
    SetIniValue(g_configPath, "idleMode", std::to_string(g_cfg.idleMode));
    SetIniValue(g_configPath, "minSizeMB", std::to_string(g_cfg.minSizeMB));
    SetIniValue(g_configPath, "resultCap", std::to_string(g_cfg.resultCap));
    std::string ex;
    for (size_t i = 0; i < g_cfg.excludePaths.size(); ++i) {
        if (i) ex += ";";
        ex += g_cfg.excludePaths[i];
    }
    SetIniValue(g_configPath, "exclude", ex);
}

// ---------------------------------------------------------------------------
// File-list pages (log / large / idle / archive / installer)
// ---------------------------------------------------------------------------

struct FilePageData {
    ScanPage page = ScanPage::Log;
    GtkWidget* vbox = nullptr;
    GtkWidget* status = nullptr;
    GtkWidget* progress = nullptr;
    GtkWidget* spinner = nullptr;
    GtkWidget* tree = nullptr;
    GtkListStore* store = nullptr;

    GtkWidget* comboScope = nullptr;
    GtkWidget* comboTime = nullptr;
    GtkWidget* checkProbe = nullptr;
    GtkWidget* checkMultiline = nullptr;
    GtkWidget* comboThreshold = nullptr;
    GtkWidget* entryCustom = nullptr;
    GtkWidget* comboIdleMode = nullptr;
    GtkWidget* comboIdleDays = nullptr;
    GtkWidget* entryIdleCustom = nullptr;

    GtkWidget* btnRefresh = nullptr;
    GtkWidget* btnSelAll = nullptr;
    GtkWidget* btnSelNone = nullptr;
    GtkWidget* btnSelInv = nullptr;
    GtkWidget* btnDelete = nullptr;
    GtkWidget* btnOpenFolder = nullptr;

    std::vector<FileItem> items;
    std::vector<unsigned char> checked;
    std::vector<unsigned> order;
    int sortCol = 3;
    bool sortAsc = false;
    bool scanning = false;
};

FilePageData g_pages[5]; // Log, Large, Idle, Archive, Installer
bool g_scanRunning = false; // UI thread only
GtkWidget* g_notebookWidget = nullptr;

FilePageData* PageFor(ScanPage p) {
    return &g_pages[(int)p];
}

ScanOptions MakeOptions(FilePageData* pg) {
    ScanOptions o;
    o.scope = g_cfg.scope;
    o.timeDays = g_cfg.logTimeDays;
    o.probe = g_cfg.probe;
    o.onlyMultiline = g_cfg.onlyMultiline;
    o.minSizeBytes = g_cfg.minSizeMB * 1024ULL * 1024ULL;
    o.idleDays = g_cfg.idleDays;
    o.idleMode = g_cfg.idleMode;
    o.resultCap = g_cfg.resultCap;
    o.excludePaths = g_cfg.excludePaths;

    if (pg->comboScope) {
        int s = gtk_combo_box_get_active(GTK_COMBO_BOX(pg->comboScope));
        o.scope = s == 1 ? 1 : 0;
    }
    if (pg->comboTime) {
        static const int days[] = { 0, 1, 7, 30 };
        int sel = gtk_combo_box_get_active(GTK_COMBO_BOX(pg->comboTime));
        o.timeDays = days[sel < 0 ? 0 : (sel > 3 ? 3 : sel)];
    }
    if (pg->checkProbe) o.probe = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(pg->checkProbe));
    if (pg->checkMultiline) o.onlyMultiline = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(pg->checkMultiline));
    if (pg->comboThreshold) {
        int sel = gtk_combo_box_get_active(GTK_COMBO_BOX(pg->comboThreshold));
        uint64_t mb = 100;
        if (sel == 0) mb = 50;
        else if (sel == 2) mb = 500;
        else if (sel == 3) mb = 1024;
        else if (sel == 4) {
            const char* t = gtk_entry_get_text(GTK_ENTRY(pg->entryCustom));
            uint64_t v = t && *t ? strtoull(t, nullptr, 10) : 100;
            mb = v ? v : 100;
        }
        o.minSizeBytes = mb * 1024ULL * 1024ULL;
    }
    if (pg->comboIdleMode) {
        int sel = gtk_combo_box_get_active(GTK_COMBO_BOX(pg->comboIdleMode));
        o.idleMode = sel == 1 ? 1 : 0;
    }
    if (pg->comboIdleDays) {
        static const int days[] = { 30, 90, 180, 365, 0 };
        int sel = gtk_combo_box_get_active(GTK_COMBO_BOX(pg->comboIdleDays));
        o.idleDays = days[sel < 0 ? 1 : (sel > 4 ? 4 : sel)];
        if (o.idleDays == 0) {
            const char* t = gtk_entry_get_text(GTK_ENTRY(pg->entryIdleCustom));
            uint64_t v = t && *t ? strtoull(t, nullptr, 10) : 90;
            o.idleDays = (int)(v ? v : 90);
        }
    }
    return o;
}

void SetStatus(FilePageData* pg, const std::string& text) {
    gtk_label_set_text(GTK_LABEL(pg->status), text.c_str());
}

void SetPageBusy(FilePageData* pg, bool busy) {
    pg->scanning = busy;
    gtk_widget_set_sensitive(pg->btnRefresh, !busy);
    gtk_widget_set_sensitive(pg->btnDelete, !busy);
    if (busy) {
        gtk_spinner_start(GTK_SPINNER(pg->spinner));
        gtk_widget_show(pg->spinner);
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(pg->progress), 0.0);
    } else {
        gtk_spinner_stop(GTK_SPINNER(pg->spinner));
        gtk_widget_hide(pg->spinner);
    }
}

// Detach + re-attach the model so all rows repaint (cheap for full refresh).
void InvalidateRows(FilePageData* pg) {
    gtk_tree_view_set_model(GTK_TREE_VIEW(pg->tree), nullptr);
    gtk_tree_view_set_model(GTK_TREE_VIEW(pg->tree), GTK_TREE_MODEL(pg->store));
}

void RebuildStore(FilePageData* pg) {
    gtk_tree_view_set_model(GTK_TREE_VIEW(pg->tree), nullptr);
    gtk_list_store_clear(pg->store);
    for (size_t i = 0; i < pg->order.size(); ++i) {
        GtkTreeIter it;
        gtk_list_store_insert_with_values(pg->store, &it, (gint)i, 0, (gint)pg->order[i], -1);
    }
    gtk_tree_view_set_model(GTK_TREE_VIEW(pg->tree), GTK_TREE_MODEL(pg->store));
}

void UpdateStatusLine(FilePageData* pg) {
    uint64_t selBytes = 0;
    size_t selCount = 0;
    for (size_t i = 0; i < pg->checked.size(); ++i) {
        if (pg->checked[i]) {
            ++selCount;
            selBytes += pg->items[i].size;
        }
    }
    char buf[512];
    snprintf(buf, sizeof(buf), "共 %zu 项 · 已勾选 %zu · 合计 %s",
             pg->order.size(), selCount, FormatBytes(selBytes).c_str());
    SetStatus(pg, buf);
}

struct ScanBox {
    FilePageData* page = nullptr;
    ScanResult res;
};

gboolean OnScanIdle(gpointer data) {
    ScanBox* box = (ScanBox*)data;
    FilePageData* pg = box->page;
    SetPageBusy(pg, false);
    g_scanRunning = false;
    if (!box->res.ok) {
        SetStatus(pg, "扫描失败：" + box->res.error);
    } else {
        pg->items = std::move(box->res.items);
        pg->checked.assign(pg->items.size(), 0);
        BuildSortedOrder(pg->items, pg->order, pg->sortCol, pg->sortAsc);
        RebuildStore(pg);
        char buf[640];
        snprintf(buf, sizeof(buf),
                 "共 %zu 项 · 已勾选 0 · 合计 0 B · 探测 %zu · 不可读 %zu · 耗时 %llu ms%s",
                 pg->order.size(), box->res.probed, box->res.unreadable,
                 (unsigned long long)box->res.elapsedMs,
                 box->res.truncated ? "（结果已达上限，不完整）" : "");
        SetStatus(pg, buf);
    }
    delete box;
    return G_SOURCE_REMOVE;
}

void StartPageScan(FilePageData* pg) {
    if (g_scanRunning || pg->scanning) return;
    g_scanRunning = true;
    SetPageBusy(pg, true);
    SetStatus(pg, "正在扫描…");
    ScanOptions opts = MakeOptions(pg);
    StartScan(pg->page, opts, [pg](ScanResult res) {
        ScanBox* box = new ScanBox();
        box->page = pg;
        box->res = std::move(res);
        g_idle_add(OnScanIdle, box);
    });
}

void SetAllChecked(FilePageData* pg, bool value) {
    for (auto& c : pg->checked) c = value ? 1 : 0;
    InvalidateRows(pg);
    UpdateStatusLine(pg);
}

void InvertChecked(FilePageData* pg) {
    for (auto& c : pg->checked) c = c ? 0 : 1;
    InvalidateRows(pg);
    UpdateStatusLine(pg);
}

// ---- forward decls used by the page builders below ----
void OnDeleteClicked(FilePageData* pg);
void OnOpenFolderClicked(FilePageData* pg);

// ---- cell rendering ----

void CellDataText(GtkTreeViewColumn* col, GtkCellRenderer* renderer,
                  GtkTreeModel* model, GtkTreeIter* iter, gpointer fieldPtr) {
    gint dataIdx = -1;
    gtk_tree_model_get(model, iter, 0, &dataIdx, -1);
    int field = GPOINTER_TO_INT(fieldPtr);
    FilePageData* pg = (FilePageData*)g_object_get_data(G_OBJECT(col), "page");
    if (dataIdx < 0 || !pg || (size_t)dataIdx >= pg->items.size()) {
        g_object_set(renderer, "text", "", nullptr);
        return;
    }
    const FileItem& fi = pg->items[dataIdx];
    std::string text;
    switch (field) {
    case 1: text = FileNameFromPath(fi.path); break;
    case 2: text = DirFromPath(fi.path); break;
    case 3: text = FormatBytes(fi.size); break;
    case 4: text = FormatTime(fi.modified); break;
    case 5:
        text = fi.kind == FileKind::KnownLog ? "日志文件" :
               (fi.kind == FileKind::ProbedText ? "文本（探针）" : "");
        break;
    }
    g_object_set(renderer, "text", text.c_str(), nullptr);
}

void CellDataToggle(GtkTreeViewColumn* col, GtkCellRenderer* renderer,
                    GtkTreeModel* model, GtkTreeIter* iter, gpointer) {
    gint dataIdx = -1;
    gtk_tree_model_get(model, iter, 0, &dataIdx, -1);
    FilePageData* pg = (FilePageData*)g_object_get_data(G_OBJECT(col), "page");
    bool on = pg && dataIdx >= 0 && (size_t)dataIdx < pg->checked.size() && pg->checked[dataIdx];
    g_object_set(renderer, "active", on ? TRUE : FALSE, nullptr);
}

void OnToggleToggled(GtkCellRendererToggle* renderer, gchar* pathStr, gpointer userData) {
    FilePageData* pg = (FilePageData*)userData;
    GtkTreePath* path = gtk_tree_path_new_from_string(pathStr);
    GtkTreeIter iter;
    if (gtk_tree_model_get_iter(GTK_TREE_MODEL(pg->store), &iter, path)) {
        gint dataIdx = -1;
        gtk_tree_model_get(GTK_TREE_MODEL(pg->store), &iter, 0, &dataIdx, -1);
        if (dataIdx >= 0 && (size_t)dataIdx < pg->checked.size()) {
            pg->checked[dataIdx] = pg->checked[dataIdx] ? 0 : 1;
            gtk_list_store_set(pg->store, &iter, 0, dataIdx, -1);
            UpdateStatusLine(pg);
        }
    }
    gtk_tree_path_free(path);
    (void)renderer;
}

void OnColumnClicked(GtkTreeViewColumn* col, gpointer userData) {
    FilePageData* pg = (FilePageData*)userData;
    if (pg->scanning) return;
    int colNo = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(col), "colId"));
    if (colNo < 0 || colNo > 4) return;
    if (colNo == 5) return; // log type column: not sortable
    if (pg->sortCol == colNo) {
        pg->sortAsc = !pg->sortAsc;
    } else {
        pg->sortCol = colNo;
        pg->sortAsc = !(colNo == 3 || colNo == 4); // size/time start descending
    }
    BuildSortedOrder(pg->items, pg->order, pg->sortCol, pg->sortAsc);
    RebuildStore(pg);
    GList* cols = gtk_tree_view_get_columns(GTK_TREE_VIEW(pg->tree));
    for (GList* l = cols; l; l = l->next) {
        GtkTreeViewColumn* c = GTK_TREE_VIEW_COLUMN(l->data);
        int id = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(c), "colId"));
        if (id == pg->sortCol) {
            gtk_tree_view_column_set_sort_indicator(c, TRUE);
            gtk_tree_view_column_set_sort_order(
                c, pg->sortAsc ? GTK_SORT_ASCENDING : GTK_SORT_DESCENDING);
        } else {
            gtk_tree_view_column_set_sort_indicator(c, FALSE);
        }
    }
    g_list_free(cols);
    UpdateStatusLine(pg);
}

GtkWidget* MakeToolbarButton(const char* label, GCallback cb, gpointer data) {
    GtkWidget* b = gtk_button_new_with_label(label);
    g_signal_connect(b, "clicked", cb, data);
    return b;
}

void AppendTextColumn(FilePageData* pg, GtkWidget* tree, const char* title,
                      int width, int field, bool sortable, int colId) {
    GtkCellRenderer* r = gtk_cell_renderer_text_new();
    GtkTreeViewColumn* col = gtk_tree_view_column_new();
    gtk_tree_view_column_set_title(col, title);
    gtk_tree_view_column_set_fixed_width(col, width);
    gtk_tree_view_column_set_sizing(col, GTK_TREE_VIEW_COLUMN_FIXED);
    gtk_tree_view_column_pack_start(col, r, TRUE);
    g_object_set_data(G_OBJECT(col), "page", pg);
    g_object_set_data(G_OBJECT(col), "colId", GINT_TO_POINTER(colId));
    gtk_tree_view_column_set_cell_data_func(col, r, CellDataText,
                                            GINT_TO_POINTER(field), nullptr);
    if (sortable) {
        gtk_tree_view_column_set_clickable(col, TRUE);
        g_signal_connect(col, "clicked", G_CALLBACK(OnColumnClicked), pg);
    }
    gtk_tree_view_append_column(GTK_TREE_VIEW(tree), col);
}

void BuildFilePage(FilePageData* pg, GtkNotebook* nb, const char* title,
                   bool logPage) {
    pg->vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);

    // top toolbar
    GtkWidget* top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_pack_start(GTK_BOX(pg->vbox), top, FALSE, FALSE, 0);

    if (logPage) {
        pg->comboScope = gtk_combo_box_text_new();
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(pg->comboScope), "全盘");
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(pg->comboScope), "用户目录");
        gtk_combo_box_set_active(GTK_COMBO_BOX(pg->comboScope), g_cfg.scope ? 1 : 0);
        gtk_box_pack_start(GTK_BOX(top), pg->comboScope, FALSE, FALSE, 0);

        pg->comboTime = gtk_combo_box_text_new();
        const char* times[] = { "全部", "最近 1 天", "最近 7 天", "最近 30 天" };
        for (const char* t : times) {
            gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(pg->comboTime), t);
        }
        int tSel = 0;
        if (g_cfg.logTimeDays == 1) tSel = 1;
        else if (g_cfg.logTimeDays == 7) tSel = 2;
        else if (g_cfg.logTimeDays == 30) tSel = 3;
        gtk_combo_box_set_active(GTK_COMBO_BOX(pg->comboTime), tSel);
        gtk_box_pack_start(GTK_BOX(top), pg->comboTime, FALSE, FALSE, 0);

        pg->checkProbe = gtk_check_button_new_with_label("探测无后缀/未知后缀文件");
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(pg->checkProbe), g_cfg.probe);
        gtk_box_pack_start(GTK_BOX(top), pg->checkProbe, FALSE, FALSE, 0);

        pg->checkMultiline = gtk_check_button_new_with_label("仅显示含换行的文本");
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(pg->checkMultiline), g_cfg.onlyMultiline);
        gtk_box_pack_start(GTK_BOX(top), pg->checkMultiline, FALSE, FALSE, 0);
    } else if (pg->page == ScanPage::Large) {
        pg->comboThreshold = gtk_combo_box_text_new();
        const char* sizes[] = { "50 MB", "100 MB", "500 MB", "1 GB", "自定义" };
        for (const char* s : sizes) {
            gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(pg->comboThreshold), s);
        }
        int sel = 1;
        if (g_cfg.minSizeMB == 50) sel = 0;
        else if (g_cfg.minSizeMB == 500) sel = 2;
        else if (g_cfg.minSizeMB == 1024) sel = 3;
        else if (g_cfg.minSizeMB != 100) sel = 4;
        gtk_combo_box_set_active(GTK_COMBO_BOX(pg->comboThreshold), sel);
        gtk_box_pack_start(GTK_BOX(top), pg->comboThreshold, FALSE, FALSE, 0);

        pg->entryCustom = gtk_entry_new();
        gtk_entry_set_width_chars(GTK_ENTRY(pg->entryCustom), 6);
        gtk_box_pack_start(GTK_BOX(top), pg->entryCustom, FALSE, FALSE, 0);
    } else if (pg->page == ScanPage::Idle) {
        pg->comboIdleMode = gtk_combo_box_text_new();
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(pg->comboIdleMode), "文件");
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(pg->comboIdleMode), "程序（可执行文件）");
        gtk_combo_box_set_active(GTK_COMBO_BOX(pg->comboIdleMode), g_cfg.idleMode ? 1 : 0);
        gtk_box_pack_start(GTK_BOX(top), pg->comboIdleMode, FALSE, FALSE, 0);

        pg->comboIdleDays = gtk_combo_box_text_new();
        const char* days[] = { "30 天", "90 天", "180 天", "365 天", "自定义" };
        for (const char* d : days) {
            gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(pg->comboIdleDays), d);
        }
        int dSel = 1;
        if (g_cfg.idleDays == 30) dSel = 0;
        else if (g_cfg.idleDays == 180) dSel = 2;
        else if (g_cfg.idleDays == 365) dSel = 3;
        else if (g_cfg.idleDays != 90) dSel = 4;
        gtk_combo_box_set_active(GTK_COMBO_BOX(pg->comboIdleDays), dSel);
        gtk_box_pack_start(GTK_BOX(top), pg->comboIdleDays, FALSE, FALSE, 0);

        pg->entryIdleCustom = gtk_entry_new();
        gtk_entry_set_width_chars(GTK_ENTRY(pg->entryIdleCustom), 6);
        gtk_box_pack_start(GTK_BOX(top), pg->entryIdleCustom, FALSE, FALSE, 0);
    }

    pg->btnRefresh = MakeToolbarButton("刷新", G_CALLBACK(+[](GtkWidget*, gpointer d) {
                                          StartPageScan((FilePageData*)d);
                                      }),
                                       pg);
    gtk_box_pack_end(GTK_BOX(top), pg->btnRefresh, FALSE, FALSE, 0);

    // selection toolbar
    GtkWidget* sel = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_pack_start(GTK_BOX(pg->vbox), sel, FALSE, FALSE, 0);
    pg->btnSelAll = MakeToolbarButton("全选", G_CALLBACK(+[](GtkWidget*, gpointer d) {
                                          SetAllChecked((FilePageData*)d, true);
                                      }),
                                      pg);
    pg->btnSelNone = MakeToolbarButton("全不选", G_CALLBACK(+[](GtkWidget*, gpointer d) {
                                           SetAllChecked((FilePageData*)d, false);
                                       }),
                                       pg);
    pg->btnSelInv = MakeToolbarButton("反选", G_CALLBACK(+[](GtkWidget*, gpointer d) {
                                          InvertChecked((FilePageData*)d);
                                      }),
                                      pg);
    pg->btnDelete = MakeToolbarButton("删除到回收站", G_CALLBACK(+[](GtkWidget*, gpointer d) {
                                          OnDeleteClicked((FilePageData*)d);
                                      }),
                                      pg);
    pg->btnOpenFolder = MakeToolbarButton("打开所在文件夹", G_CALLBACK(+[](GtkWidget*, gpointer d) {
                                              OnOpenFolderClicked((FilePageData*)d);
                                          }),
                                          pg);
    gtk_box_pack_start(GTK_BOX(sel), pg->btnSelAll, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(sel), pg->btnSelNone, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(sel), pg->btnSelInv, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(sel), pg->btnDelete, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(sel), pg->btnOpenFolder, FALSE, FALSE, 0);

    // tree
    pg->store = gtk_list_store_new(1, G_TYPE_INT);
    pg->tree = gtk_tree_view_new_with_model(GTK_TREE_MODEL(pg->store));
    gtk_tree_view_set_headers_clickable(GTK_TREE_VIEW(pg->tree), TRUE);
    gtk_tree_view_set_rules_hint(GTK_TREE_VIEW(pg->tree), TRUE);

    GtkCellRenderer* tog = gtk_cell_renderer_toggle_new();
    gtk_cell_renderer_toggle_set_activatable(GTK_CELL_RENDERER_TOGGLE(tog), TRUE);
    GtkTreeViewColumn* tcol = gtk_tree_view_column_new();
    gtk_tree_view_column_set_title(tcol, "");
    gtk_tree_view_column_set_fixed_width(tcol, 34);
    gtk_tree_view_column_set_sizing(tcol, GTK_TREE_VIEW_COLUMN_FIXED);
    gtk_tree_view_column_pack_start(tcol, tog, FALSE);
    g_object_set_data(G_OBJECT(tcol), "page", pg);
    gtk_tree_view_column_set_cell_data_func(tcol, tog, CellDataToggle, nullptr, nullptr);
    gtk_tree_view_append_column(GTK_TREE_VIEW(pg->tree), tcol);
    g_signal_connect(tog, "toggled", G_CALLBACK(OnToggleToggled), pg);

    AppendTextColumn(pg, pg->tree, "名称", 260, 1, true, 1);
    AppendTextColumn(pg, pg->tree, "位置", 420, 2, true, 2);
    AppendTextColumn(pg, pg->tree, "大小", 100, 3, true, 3);
    AppendTextColumn(pg, pg->tree, "修改时间", 150, 4, true, 4);
    if (logPage) AppendTextColumn(pg, pg->tree, "类型", 110, 5, true, 5);

    GtkWidget* scroll = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(scroll), pg->tree);
    gtk_box_pack_start(GTK_BOX(pg->vbox), scroll, TRUE, TRUE, 0);

    // status row
    GtkWidget* statusRow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_pack_start(GTK_BOX(pg->vbox), statusRow, FALSE, FALSE, 0);
    pg->status = gtk_label_new("点击“刷新”开始扫描");
    gtk_label_set_xalign(GTK_LABEL(pg->status), 0.0);
    gtk_box_pack_start(GTK_BOX(statusRow), pg->status, TRUE, TRUE, 0);
    pg->spinner = gtk_spinner_new();
    gtk_box_pack_start(GTK_BOX(statusRow), pg->spinner, FALSE, FALSE, 0);
    gtk_widget_set_no_show_all(pg->spinner, TRUE);
    pg->progress = gtk_progress_bar_new();
    gtk_widget_set_size_request(pg->progress, 160, -1);
    gtk_box_pack_start(GTK_BOX(statusRow), pg->progress, FALSE, FALSE, 0);

    pg->order.clear();
    gtk_widget_show_all(pg->vbox);
    gtk_notebook_append_page(nb, pg->vbox, gtk_label_new(title));
}

// ---- delete ----

struct DeleteBox {
    FilePageData* page = nullptr;
    TrashStats stats;
    std::vector<std::string> paths; // permanent-delete retry list
    bool phase = 0;                 // 0 trash done, 1 permanent done
};

struct ProgressBox {
    FilePageData* page = nullptr;
    size_t done = 0;
    size_t total = 0;
};

gboolean OnProgressIdle(gpointer data) {
    ProgressBox* pb = (ProgressBox*)data;
    if (pb->total) gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(pb->page->progress),
                                                 (double)pb->done / pb->total);
    delete pb;
    return G_SOURCE_REMOVE;
}

void PostProgress(FilePageData* pg, size_t done, size_t total) {
    if (done % 64 != 0 && done != total) return;
    ProgressBox* pb = new ProgressBox();
    pb->page = pg;
    pb->done = done;
    pb->total = total;
    g_idle_add(OnProgressIdle, pb);
}

gboolean OnDeleteIdle(gpointer data) {
    DeleteBox* box = (DeleteBox*)data;
    FilePageData* pg = box->page;
    SetPageBusy(pg, false);

    if (box->phase == 0) {
        // remove trashed (ok + permanent) items from the page
        std::set<std::string> failed(box->stats.failedPaths.begin(), box->stats.failedPaths.end());
        FileVec newItems;
        std::vector<unsigned char> newChecked;
        for (size_t i = 0; i < pg->items.size(); ++i) {
            if (!failed.count(pg->items[i].path)) {
                newItems.push_back(std::move(pg->items[i]));
                newChecked.push_back(pg->checked[i]);
            }
        }
        pg->items = std::move(newItems);
        pg->checked = std::move(newChecked);
        BuildSortedOrder(pg->items, pg->order, pg->sortCol, pg->sortAsc);
        RebuildStore(pg);

        char buf[256];
        snprintf(buf, sizeof(buf), "已删除 %zu 项，失败 %zu 项%s",
                 box->stats.ok + box->stats.permanent, box->stats.failed,
                 box->stats.permanent ? "（含永久删除）" : "");
        SetStatus(pg, buf);

        if (!box->stats.failedPaths.empty()) {
            GtkWidget* dlg = gtk_message_dialog_new(
                GTK_WINDOW(g_window), GTK_DIALOG_MODAL, GTK_MESSAGE_WARNING,
                GTK_BUTTONS_YES_NO,
                "%zu 个文件无法移入回收站（可能位于无法回收的分区）。是否永久删除？",
                box->stats.failedPaths.size());
            gint r = gtk_dialog_run(GTK_DIALOG(dlg));
            gtk_widget_destroy(dlg);
            if (r == GTK_RESPONSE_YES) {
                std::vector<std::string> paths = box->stats.failedPaths;
                box->phase = 1;
                SetPageBusy(pg, true);
                SetStatus(pg, "正在永久删除…");
                std::thread([pg, paths] {
                    TrashStats s = DeletePathsPermanently(
                        paths, [pg](size_t d, size_t t) { PostProgress(pg, d, t); });
                    DeleteBox* nb = new DeleteBox();
                    nb->page = pg;
                    nb->stats = std::move(s);
                    nb->phase = 1;
                    g_idle_add(OnDeleteIdle, nb);
                }).detach();
                delete box;
                return G_SOURCE_REMOVE;
            }
        }
        UpdateStatusLine(pg);
    } else {
        std::set<std::string> failed(box->stats.failedPaths.begin(), box->stats.failedPaths.end());
        FileVec newItems;
        std::vector<unsigned char> newChecked;
        for (size_t i = 0; i < pg->items.size(); ++i) {
            if (!failed.count(pg->items[i].path)) {
                newItems.push_back(std::move(pg->items[i]));
                newChecked.push_back(pg->checked[i]);
            }
        }
        pg->items = std::move(newItems);
        pg->checked = std::move(newChecked);
        BuildSortedOrder(pg->items, pg->order, pg->sortCol, pg->sortAsc);
        RebuildStore(pg);
        char buf[256];
        snprintf(buf, sizeof(buf), "永久删除完成：成功 %zu，失败 %zu",
                 box->stats.permanent, box->stats.failed);
        SetStatus(pg, buf);
        UpdateStatusLine(pg);
    }
    delete box;
    return G_SOURCE_REMOVE;
}

void OnDeleteClicked(FilePageData* pg) {
    if (pg->scanning) return;
    std::vector<std::string> paths;
    uint64_t totalBytes = 0;
    for (size_t i = 0; i < pg->checked.size(); ++i) {
        if (pg->checked[i]) {
            paths.push_back(pg->items[i].path);
            totalBytes += pg->items[i].size;
        }
    }
    if (paths.empty()) {
        GtkWidget* dlg = gtk_message_dialog_new(
            GTK_WINDOW(g_window), GTK_DIALOG_MODAL, GTK_MESSAGE_INFO,
            GTK_BUTTONS_OK, "请先勾选要删除的文件。");
        gtk_dialog_run(GTK_DIALOG(dlg));
        gtk_widget_destroy(dlg);
        return;
    }

    char buf[512];
    snprintf(buf, sizeof(buf), "确定删除 %zu 个文件（合计 %s）到回收站吗？",
             paths.size(), FormatBytes(totalBytes).c_str());
    GtkWidget* dlg = gtk_message_dialog_new(
        GTK_WINDOW(g_window), GTK_DIALOG_MODAL, GTK_MESSAGE_WARNING,
        GTK_BUTTONS_YES_NO, "%s", buf);
    gint r = gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(dlg);
    if (r != GTK_RESPONSE_YES) return;

    SetPageBusy(pg, true);
    SetStatus(pg, "正在删除到回收站…");
    std::thread([pg, paths = std::move(paths)] {
        TrashStats stats = DeletePathsToTrash(
            paths, [pg](size_t d, size_t t) { PostProgress(pg, d, t); });
        DeleteBox* box = new DeleteBox();
        box->page = pg;
        box->stats = std::move(stats);
        box->phase = 0;
        g_idle_add(OnDeleteIdle, box);
    }).detach();
}

void OnOpenFolderClicked(FilePageData* pg) {
    GtkTreeSelection* sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(pg->tree));
    GtkTreeModel* model = nullptr;
    GtkTreeIter iter;
    if (!gtk_tree_selection_get_selected(sel, &model, &iter)) return;
    gint dataIdx = -1;
    gtk_tree_model_get(model, &iter, 0, &dataIdx, -1);
    if (dataIdx < 0 || (size_t)dataIdx >= pg->items.size()) return;
    std::string dir = DirFromPath(pg->items[dataIdx].path);
    if (dir.empty()) return;
    GError* err = nullptr;
    char* uri = g_filename_to_uri(dir.c_str(), nullptr, &err);
    if (uri) {
        gtk_show_uri_on_window(GTK_WINDOW(g_window), uri, GDK_CURRENT_TIME, &err);
        g_free(uri);
    }
    if (err) {
        GtkWidget* dlg = gtk_message_dialog_new(
            GTK_WINDOW(g_window), GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR,
            GTK_BUTTONS_OK, "无法打开文件夹：%s", err->message);
        gtk_dialog_run(GTK_DIALOG(dlg));
        gtk_widget_destroy(dlg);
        g_error_free(err);
    }
}

// ---------------------------------------------------------------------------
// System info page
// ---------------------------------------------------------------------------

struct SysPage {
    GtkWidget* tree = nullptr;
    GtkListStore* store = nullptr;
};
SysPage g_sys;
std::vector<SysEntry> g_sysHw; // static hardware rows, refreshed by the button

void UpdateSysLive() {
    std::vector<SysEntry> live;
    CollectLiveInfo(live);
    std::vector<std::pair<std::string, double>> temps;
    CollectTemperatures(temps);

    gtk_list_store_clear(g_sys.store);
    GtkTreeIter it;
    auto add = [&](const std::string& n, const std::string& v) {
        gtk_list_store_append(g_sys.store, &it);
        gtk_list_store_set(g_sys.store, &it, 0, n.c_str(), 1, v.c_str(), -1);
    };
    for (auto& e : g_sysHw) add(e.name, e.value);
    for (auto& e : live) add(e.name, e.value);
    for (auto& t : temps) {
        char buf[64];
        snprintf(buf, sizeof(buf), "%.1f ℃", t.second);
        add("温度：" + t.first, buf);
    }
}

void RebuildSysInfo() {
    CollectHardwareInfo(g_sysHw);
    UpdateSysLive();
}

gboolean OnSysTick(gpointer) {
    if (g_notebookWidget) {
        int cur = gtk_notebook_get_current_page(GTK_NOTEBOOK(g_notebookWidget));
        if (cur != 6) return G_SOURCE_CONTINUE; // 系统信息 tab
    }
    UpdateSysLive();
    return G_SOURCE_CONTINUE;
}

// ---------------------------------------------------------------------------
// Memory page
// ---------------------------------------------------------------------------

struct MemPage {
    GtkWidget* info = nullptr;
    GtkWidget* tree = nullptr;
    GtkListStore* store = nullptr;
};
MemPage g_mem;

void RefreshMemInfoLabel() {
    MemInfo m;
    if (!GetMemInfo(m)) {
        gtk_label_set_text(GTK_LABEL(g_mem.info), "无法读取内存信息");
        return;
    }
    double usedPct = m.total ? (double)(m.total - m.available) / m.total * 100.0 : 0.0;
    char buf[512];
    snprintf(buf, sizeof(buf),
             "物理内存总量：%s\n可用物理内存：%s（已用 %.1f%%）\n"
             "提交限制：%s，已提交：%s",
             FormatBytes(m.total).c_str(), FormatBytes(m.available).c_str(), usedPct,
             FormatBytes(m.commitLimit).c_str(), FormatBytes(m.committed).c_str());
    gtk_label_set_text(GTK_LABEL(g_mem.info), buf);
}

struct ProcBox {
    std::vector<ProcEntry> procs;
};

gboolean OnProcIdle(gpointer data) {
    ProcBox* box = (ProcBox*)data;
    gtk_list_store_clear(g_mem.store);
    GtkTreeIter it;
    for (auto& p : box->procs) {
        gtk_list_store_append(g_mem.store, &it);
        gtk_list_store_set(g_mem.store, &it, 0, p.name.c_str(), 1, p.pid,
                           2, FormatBytes(p.rss).c_str(), -1);
    }
    delete box;
    return G_SOURCE_REMOVE;
}

void RefreshProcesses() {
    std::thread([] {
        ProcBox* box = new ProcBox();
        EnumProcesses(box->procs);
        g_idle_add(OnProcIdle, box);
    }).detach();
}

struct SimpleResultBox {
    std::string text;
    bool error = false;
};

gboolean OnSimpleResultIdle(gpointer data) {
    SimpleResultBox* box = (SimpleResultBox*)data;
    GtkWidget* dlg = gtk_message_dialog_new(
        GTK_WINDOW(g_window), GTK_DIALOG_MODAL,
        box->error ? GTK_MESSAGE_ERROR : GTK_MESSAGE_INFO,
        GTK_BUTTONS_OK, "%s", box->text.c_str());
    gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(dlg);
    delete box;
    return G_SOURCE_REMOVE;
}

void OnDropCaches(GtkWidget*, gpointer) {
    GtkWidget* dlg = gtk_message_dialog_new(
        GTK_WINDOW(g_window), GTK_DIALOG_MODAL, GTK_MESSAGE_WARNING,
        GTK_BUTTONS_YES_NO,
        "清理内存缓存需要管理员权限，将弹出授权窗口。继续吗？");
    gint r = gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(dlg);
    if (r != GTK_RESPONSE_YES) return;
    std::thread([] {
        bool ok = RunDropCaches();
        SimpleResultBox* box = new SimpleResultBox();
        box->error = !ok;
        box->text = ok ? "缓存清理完成。" : "清理失败（授权被取消或权限不足）。";
        g_idle_add(OnSimpleResultIdle, box);
    }).detach();
}

void OnKillProcess(GtkWidget*, gpointer) {
    GtkTreeSelection* sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(g_mem.tree));
    GtkTreeModel* model = nullptr;
    GtkTreeIter iter;
    if (!gtk_tree_selection_get_selected(sel, &model, &iter)) return;
    gchar* name = nullptr;
    gint pid = 0;
    gtk_tree_model_get(model, &iter, 0, &name, 1, &pid, -1);
    char buf[256];
    snprintf(buf, sizeof(buf), "确定结束进程“%s”（PID %d）吗？未保存的数据可能丢失。",
             name ? name : "", pid);
    g_free(name);
    GtkWidget* dlg = gtk_message_dialog_new(
        GTK_WINDOW(g_window), GTK_DIALOG_MODAL, GTK_MESSAGE_WARNING,
        GTK_BUTTONS_YES_NO, "%s", buf);
    gint r = gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(dlg);
    if (r != GTK_RESPONSE_YES) return;
    if (!KillProcess(pid)) {
        GtkWidget* dlg2 = gtk_message_dialog_new(
            GTK_WINDOW(g_window), GTK_DIALOG_MODAL, GTK_MESSAGE_WARNING,
            GTK_BUTTONS_YES_NO, "权限不足，是否以管理员身份强制结束（pkexec kill -9）？");
        gint r2 = gtk_dialog_run(GTK_DIALOG(dlg2));
        gtk_widget_destroy(dlg2);
        if (r2 == GTK_RESPONSE_YES) {
            std::thread([pid] {
                std::string cmd = "pkexec kill -9 " + std::to_string(pid) + " 2>&1";
                FILE* f = popen(cmd.c_str(), "r");
                if (f) pclose(f);
            }).detach();
        }
    }
    RefreshProcesses();
}

// ---------------------------------------------------------------------------
// Uninstall page
// ---------------------------------------------------------------------------

struct AppsPage {
    GtkWidget* tree = nullptr;
    GtkListStore* store = nullptr;
    std::vector<AppEntry> apps;
};
AppsPage g_apps;

struct AppsBox {
    std::vector<AppEntry> apps;
};

gboolean OnAppsIdle(gpointer data) {
    AppsBox* box = (AppsBox*)data;
    gtk_list_store_clear(g_apps.store);
    g_apps.apps = std::move(box->apps);
    GtkTreeIter it;
    for (size_t i = 0; i < g_apps.apps.size(); ++i) {
        AppEntry& a = g_apps.apps[i];
        std::string sizeText = a.sizeKB ? FormatBytes(a.sizeKB * 1024) : "";
        gtk_list_store_append(g_apps.store, &it);
        gtk_list_store_set(g_apps.store, &it, 0, a.name.c_str(), 1, a.version.c_str(),
                           2, a.source.c_str(), 3, sizeText.c_str(),
                           4, (gint)i, -1);
    }
    delete box;
    return G_SOURCE_REMOVE;
}

void RefreshApps() {
    std::thread([] {
        AppsBox* box = new AppsBox();
        EnumInstalledApps(box->apps);
        g_idle_add(OnAppsIdle, box);
    }).detach();
}

void OnUninstall(GtkWidget*, gpointer) {
    GtkTreeSelection* sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(g_apps.tree));
    GtkTreeModel* model = nullptr;
    GtkTreeIter iter;
    if (!gtk_tree_selection_get_selected(sel, &model, &iter)) return;
    gint row = -1;
    gtk_tree_model_get(model, &iter, 4, &row, -1);
    if (row < 0 || (size_t)row >= g_apps.apps.size()) return;
    const AppEntry& app = g_apps.apps[row];
    char buf[256];
    snprintf(buf, sizeof(buf), "确定卸载“%s”（%s）吗？将弹出授权窗口。",
             app.name.c_str(), app.source.c_str());
    GtkWidget* dlg = gtk_message_dialog_new(
        GTK_WINDOW(g_window), GTK_DIALOG_MODAL, GTK_MESSAGE_WARNING,
        GTK_BUTTONS_YES_NO, "%s", buf);
    gint r = gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(dlg);
    if (r != GTK_RESPONSE_YES) return;

    std::thread([app] {
        std::string err;
        bool ok = UninstallApp(app, err);
        SimpleResultBox* box = new SimpleResultBox();
        box->error = !ok;
        box->text = ok ? "卸载命令已执行完成。" : ("卸载失败：" + err);
        g_idle_add(OnSimpleResultIdle, box);
    }).detach();
}

// ---------------------------------------------------------------------------
// Settings page
// ---------------------------------------------------------------------------

struct SettingsPage {
    GtkWidget* checkProbe = nullptr;
    GtkWidget* checkMultiline = nullptr;
    GtkWidget* entryResultCap = nullptr;
    GtkWidget* textExclude = nullptr;
};
SettingsPage g_set;

void OnSettingsSave(GtkWidget*, gpointer) {
    g_cfg.probe = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(g_set.checkProbe));
    g_cfg.onlyMultiline = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(g_set.checkMultiline));
    const char* cap = gtk_entry_get_text(GTK_ENTRY(g_set.entryResultCap));
    size_t v = cap && *cap ? (size_t)strtoull(cap, nullptr, 10) : 0;
    if (v) g_cfg.resultCap = v;

    g_cfg.excludePaths.clear();
    GtkTextBuffer* buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(g_set.textExclude));
    GtkTextIter s, e;
    gtk_text_buffer_get_start_iter(buf, &s);
    gtk_text_buffer_get_end_iter(buf, &e);
    char* text = gtk_text_buffer_get_text(buf, &s, &e, FALSE);
    for (auto& line : SplitLines(text ? text : "")) {
        std::string p = Trim(line);
        if (!p.empty()) g_cfg.excludePaths.push_back(p);
    }
    g_free(text);
    SaveConfig();
    GtkWidget* dlg = gtk_message_dialog_new(
        GTK_WINDOW(g_window), GTK_DIALOG_MODAL, GTK_MESSAGE_INFO,
        GTK_BUTTONS_OK, "设置已保存。");
    gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(dlg);
}

void BuildSettingsPage(GtkNotebook* nb) {
    GtkWidget* vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    g_set.checkProbe = gtk_check_button_new_with_label("启用未知格式文本探针");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_set.checkProbe), g_cfg.probe);
    g_set.checkMultiline = gtk_check_button_new_with_label("仅显示含换行的文本");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_set.checkMultiline), g_cfg.onlyMultiline);
    gtk_box_pack_start(GTK_BOX(vbox), g_set.checkProbe, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), g_set.checkMultiline, FALSE, FALSE, 0);

    GtkWidget* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_pack_start(GTK_BOX(vbox), row, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), gtk_label_new("结果上限："), FALSE, FALSE, 0);
    g_set.entryResultCap = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(g_set.entryResultCap), std::to_string(g_cfg.resultCap).c_str());
    gtk_entry_set_width_chars(GTK_ENTRY(g_set.entryResultCap), 10);
    gtk_box_pack_start(GTK_BOX(row), g_set.entryResultCap, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(vbox), gtk_label_new("排除路径（每行一个，前缀匹配）："),
                       FALSE, FALSE, 0);
    GtkWidget* scroll = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_size_request(scroll, -1, 180);
    g_set.textExclude = gtk_text_view_new();
    gtk_container_add(GTK_CONTAINER(scroll), g_set.textExclude);
    gtk_box_pack_start(GTK_BOX(vbox), scroll, TRUE, TRUE, 0);

    GtkTextBuffer* buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(g_set.textExclude));
    std::string ex;
    for (size_t i = 0; i < g_cfg.excludePaths.size(); ++i) {
        if (i) ex += "\n";
        ex += g_cfg.excludePaths[i];
    }
    gtk_text_buffer_set_text(buf, ex.c_str(), -1);

    GtkWidget* save = MakeToolbarButton("保存设置", G_CALLBACK(OnSettingsSave), nullptr);
    gtk_box_pack_start(GTK_BOX(vbox), save, FALSE, FALSE, 0);

    gtk_widget_show_all(vbox);
    gtk_notebook_append_page(nb, vbox, gtk_label_new("设置"));
}

// ---------------------------------------------------------------------------
// Main window
// ---------------------------------------------------------------------------

void BuildSysInfoPage(GtkNotebook* nb) {
    GtkWidget* vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget* top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_pack_start(GTK_BOX(vbox), top, FALSE, FALSE, 0);
    GtkWidget* refresh = MakeToolbarButton("刷新", G_CALLBACK(+[](GtkWidget*, gpointer) {
                                                RebuildSysInfo();
                                            }),
                                           nullptr);
    gtk_box_pack_end(GTK_BOX(top), refresh, FALSE, FALSE, 0);

    g_sys.store = gtk_list_store_new(2, G_TYPE_STRING, G_TYPE_STRING);
    g_sys.tree = gtk_tree_view_new_with_model(GTK_TREE_MODEL(g_sys.store));
    GtkCellRenderer* r1 = gtk_cell_renderer_text_new();
    GtkTreeViewColumn* c1 = gtk_tree_view_column_new_with_attributes("项目", r1, "text", 0, nullptr);
    gtk_tree_view_column_set_fixed_width(c1, 200);
    gtk_tree_view_column_set_sizing(c1, GTK_TREE_VIEW_COLUMN_FIXED);
    gtk_tree_view_append_column(GTK_TREE_VIEW(g_sys.tree), c1);
    GtkCellRenderer* r2 = gtk_cell_renderer_text_new();
    GtkTreeViewColumn* c2 = gtk_tree_view_column_new_with_attributes("值", r2, "text", 1, nullptr);
    gtk_tree_view_append_column(GTK_TREE_VIEW(g_sys.tree), c2);

    GtkWidget* scroll = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(scroll), g_sys.tree);
    gtk_box_pack_start(GTK_BOX(vbox), scroll, TRUE, TRUE, 0);

    gtk_widget_show_all(vbox);
    gtk_notebook_append_page(nb, vbox, gtk_label_new("系统信息"));
    RebuildSysInfo();
}

void BuildMemPage(GtkNotebook* nb) {
    GtkWidget* vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    g_mem.info = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(g_mem.info), 0.0);
    gtk_box_pack_start(GTK_BOX(vbox), g_mem.info, FALSE, FALSE, 0);

    GtkWidget* top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_pack_start(GTK_BOX(vbox), top, FALSE, FALSE, 0);
    GtkWidget* refresh = MakeToolbarButton("刷新", G_CALLBACK(+[](GtkWidget*, gpointer) {
                                                RefreshMemInfoLabel();
                                                RefreshProcesses();
                                            }),
                                           nullptr);
    GtkWidget* drop = MakeToolbarButton("清理内存缓存（需授权）", G_CALLBACK(OnDropCaches), nullptr);
    GtkWidget* kill = MakeToolbarButton("结束进程", G_CALLBACK(OnKillProcess), nullptr);
    gtk_box_pack_start(GTK_BOX(top), refresh, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(top), drop, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(top), kill, FALSE, FALSE, 0);

    g_mem.store = gtk_list_store_new(3, G_TYPE_STRING, G_TYPE_INT, G_TYPE_STRING);
    g_mem.tree = gtk_tree_view_new_with_model(GTK_TREE_MODEL(g_mem.store));
    const char* titles[] = { "进程", "PID", "内存占用" };
    for (int i = 0; i < 3; ++i) {
        GtkCellRenderer* r = gtk_cell_renderer_text_new();
        GtkTreeViewColumn* c = gtk_tree_view_column_new_with_attributes(titles[i], r, "text", i, nullptr);
        if (i == 0) gtk_tree_view_column_set_fixed_width(c, 300);
        gtk_tree_view_column_set_sizing(c, GTK_TREE_VIEW_COLUMN_FIXED);
        gtk_tree_view_append_column(GTK_TREE_VIEW(g_mem.tree), c);
    }
    GtkWidget* scroll = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(scroll), g_mem.tree);
    gtk_box_pack_start(GTK_BOX(vbox), scroll, TRUE, TRUE, 0);

    gtk_widget_show_all(vbox);
    gtk_notebook_append_page(nb, vbox, gtk_label_new("内存清理"));
    RefreshMemInfoLabel();
    RefreshProcesses();
}

void BuildAppsPage(GtkNotebook* nb) {
    GtkWidget* vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget* top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_pack_start(GTK_BOX(vbox), top, FALSE, FALSE, 0);
    GtkWidget* refresh = MakeToolbarButton("刷新", G_CALLBACK(+[](GtkWidget*, gpointer) {
                                                RefreshApps();
                                            }),
                                           nullptr);
    GtkWidget* uninst = MakeToolbarButton("卸载所选", G_CALLBACK(OnUninstall), nullptr);
    gtk_box_pack_start(GTK_BOX(top), refresh, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(top), uninst, FALSE, FALSE, 0);

    g_apps.store = gtk_list_store_new(5, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING,
                                      G_TYPE_STRING, G_TYPE_INT);
    g_apps.tree = gtk_tree_view_new_with_model(GTK_TREE_MODEL(g_apps.store));
    const char* titles[] = { "名称", "版本", "来源", "大小" };
    for (int i = 0; i < 4; ++i) {
        GtkCellRenderer* r = gtk_cell_renderer_text_new();
        GtkTreeViewColumn* c = gtk_tree_view_column_new_with_attributes(titles[i], r, "text", i, nullptr);
        gtk_tree_view_column_set_fixed_width(c, i == 0 ? 300 : 120);
        gtk_tree_view_column_set_sizing(c, GTK_TREE_VIEW_COLUMN_FIXED);
        gtk_tree_view_append_column(GTK_TREE_VIEW(g_apps.tree), c);
    }
    GtkWidget* scroll = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(scroll), g_apps.tree);
    gtk_box_pack_start(GTK_BOX(vbox), scroll, TRUE, TRUE, 0);

    gtk_widget_show_all(vbox);
    gtk_notebook_append_page(nb, vbox, gtk_label_new("软件卸载"));
    RefreshApps();
}

gboolean OnDeleteEvent(GtkWidget*, GdkEvent*, gpointer) {
    gtk_main_quit();
    return TRUE;
}

} // namespace

int RunApp(int argc, char** argv) {
    gtk_init(&argc, &argv);
    LoadConfig();

    g_window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(g_window), "UbuntuClean - 系统清理工具");
    gtk_window_set_default_size(GTK_WINDOW(g_window), 1024, 700);
    g_signal_connect(g_window, "delete-event", G_CALLBACK(OnDeleteEvent), nullptr);

    GtkWidget* notebook = gtk_notebook_new();
    g_notebookWidget = notebook;
    gtk_container_add(GTK_CONTAINER(g_window), notebook);

    BuildFilePage(PageFor(ScanPage::Log), GTK_NOTEBOOK(notebook), "日志清理", true);
    BuildFilePage(PageFor(ScanPage::Large), GTK_NOTEBOOK(notebook), "大文件", false);
    BuildFilePage(PageFor(ScanPage::Idle), GTK_NOTEBOOK(notebook), "久未使用", false);
    BuildFilePage(PageFor(ScanPage::Archive), GTK_NOTEBOOK(notebook), "压缩包管理", false);
    BuildFilePage(PageFor(ScanPage::Installer), GTK_NOTEBOOK(notebook), "安装包管理", false);
    BuildMemPage(GTK_NOTEBOOK(notebook));
    BuildSysInfoPage(GTK_NOTEBOOK(notebook));
    BuildAppsPage(GTK_NOTEBOOK(notebook));
    BuildSettingsPage(GTK_NOTEBOOK(notebook));

    gtk_widget_show_all(g_window);
    // 1 Hz refresh of the system-info tab while it is visible.
    g_timeout_add(1000, OnSysTick, nullptr);
    gtk_main();
    return 0;
}
