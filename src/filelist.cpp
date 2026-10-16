#include "filelist.h"

#include "scan.h"
#include "util.h"

#include <algorithm>
#include <execution>
#include <numeric>

namespace {

int CmpPath(const FileItem& a, const FileItem& b) {
    int r = CompareStringOrdinal(a.path.data(), (int)a.path.size(),
                                 b.path.data(), (int)b.path.size(), TRUE);
    if (r == CSTR_LESS_THAN) return -1;
    if (r == CSTR_GREATER_THAN) return 1;
    return 0;
}

}

int CompareFileItems(const FileItem& a, const FileItem& b, int col, bool asc, bool idleCols) {
    int r = 0;
    switch (col) {
    case 0: // checkbox column: sort by name
    case 1: {
        std::wstring na = FileNameFromPath(a.path);
        std::wstring nb = FileNameFromPath(b.path);
        int c = CompareStringOrdinal(na.data(), (int)na.size(), nb.data(), (int)nb.size(), TRUE);
        r = c == CSTR_LESS_THAN ? -1 : (c == CSTR_GREATER_THAN ? 1 : 0);
        break;
    }
    case 2: r = CmpPath(a, b); break;
    case 3:
        r = a.size < b.size ? -1 : (a.size > b.size ? 1 : 0);
        break;
    case 4: {
        uint64_t ta = FileTimeToU64(idleCols ? a.dateRun : a.modified);
        uint64_t tb = FileTimeToU64(idleCols ? b.dateRun : b.modified);
        r = ta < tb ? -1 : (ta > tb ? 1 : 0);
        break;
    }
    case 5:
        if (idleCols)
            r = a.runCount < b.runCount ? -1 : (a.runCount > b.runCount ? 1 : 0);
        else
            r = a.kind < b.kind ? -1 : (a.kind > b.kind ? 1 : 0);
        break;
    default: r = 0; break;
    }
    if (r == 0) r = CmpPath(a, b);
    return asc ? r : -r;
}

void BuildSortedOrder(const FileVec& items, std::vector<unsigned>& order, int col, bool asc,
                      bool idleCols) {
    order.resize(items.size());
    std::iota(order.begin(), order.end(), 0u);
    if (items.size() < 2) return;
    if (items.size() > 50000) {
        std::sort(std::execution::par, order.begin(), order.end(),
                  [&](unsigned i, unsigned j) {
                      return CompareFileItems(items[i], items[j], col, asc, idleCols) < 0;
                  });
    } else {
        std::sort(order.begin(), order.end(),
                  [&](unsigned i, unsigned j) {
                      return CompareFileItems(items[i], items[j], col, asc, idleCols) < 0;
                  });
    }
}
