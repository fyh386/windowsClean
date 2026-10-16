#include "filelist.h"

#include "util.h"

#include <algorithm>
#include <numeric>

namespace {

int CmpStr(const std::string& a, const std::string& b) {
    std::string la = LowerAscii(a);
    std::string lb = LowerAscii(b);
    if (la < lb) return -1;
    if (la > lb) return 1;
    return 0;
}

} // namespace

int CompareFileItems(const FileItem& a, const FileItem& b, int col, bool asc) {
    int r = 0;
    switch (col) {
    case 0:
    case 1: r = CmpStr(FileNameFromPath(a.path), FileNameFromPath(b.path)); break;
    case 2: r = CmpStr(a.path, b.path); break;
    case 3: r = a.size < b.size ? -1 : (a.size > b.size ? 1 : 0); break;
    case 4: r = a.modified < b.modified ? -1 : (a.modified > b.modified ? 1 : 0); break;
    default: r = 0; break;
    }
    if (r == 0) r = CmpStr(a.path, b.path);
    return asc ? r : -r;
}

void BuildSortedOrder(const FileVec& items, std::vector<unsigned>& order, int col, bool asc) {
    order.resize(items.size());
    std::iota(order.begin(), order.end(), 0u);
    if (items.size() < 2) return;
    std::sort(order.begin(), order.end(), [&](unsigned i, unsigned j) {
        return CompareFileItems(items[i], items[j], col, asc) < 0;
    });
}
