#include "scan.h"

#include "util.h"

#include <atomic>
#include <array>
#include <unordered_set>

namespace {

const std::unordered_set<std::wstring>& LogExts() {
    static const std::unordered_set<std::wstring> s = {
        L"log", L"trace", L"etl", L"evtx", L"log1", L"log2"
    };
    return s;
}

// Known non-log formats: executables, images, media, archives, documents,
// source, configs, databases, caches, etc.
const std::unordered_set<std::wstring>& NonLogExts() {
    static const std::unordered_set<std::wstring> s = {
        L"exe", L"dll", L"sys", L"msi", L"msp", L"com", L"scr", L"drv",
        L"bat", L"cmd", L"ps1", L"vbs", L"js", L"jse", L"wsf",
        L"c", L"cpp", L"cxx", L"h", L"hpp", L"hh", L"cs", L"java", L"jar",
        L"py", L"rb", L"go", L"rs", L"ts", L"jsx", L"tsx",
        L"html", L"htm", L"css", L"scss", L"less",
        L"xml", L"json", L"yml", L"yaml", L"toml", L"ini", L"cfg", L"conf",
        L"reg", L"inf", L"manifest", L"props", L"targets",
        L"doc", L"docx", L"xls", L"xlsx", L"ppt", L"pptx", L"pdf",
        L"epub", L"mobi", L"rtf", L"odt", L"ods", L"odp", L"md", L"tex", L"txt",
        L"jpg", L"jpeg", L"png", L"gif", L"bmp", L"tif", L"tiff", L"webp",
        L"ico", L"svg", L"psd", L"ai", L"heic", L"avif", L"raw",
        L"mp3", L"wav", L"flac", L"ogg", L"m4a", L"aac", L"wma", L"mid",
        L"mp4", L"mkv", L"avi", L"mov", L"wmv", L"flv", L"webm", L"m4v",
        L"3gp", L"mts", L"m2ts",
        L"zip", L"rar", L"7z", L"tar", L"gz", L"bz2", L"xz", L"z", L"iso",
        L"cab", L"msu", L"deb", L"rpm", L"apk", L"ipa", L"vhd", L"vhdx", L"img",
        L"db", L"sqlite", L"sqlite3", L"mdb", L"accdb", L"dbf", L"mdf", L"ldf",
        L"pst", L"ost", L"bak", L"dmp", L"tmp", L"temp", L"cache", L"dat",
        L"pdb", L"obj", L"lib", L"a", L"so", L"o", L"bin",
        L"lnk", L"url", L"nls", L"efi", L"mui", L"hlp", L"chm", L"ttf",
        L"otf", L"fon", L"wim", L"esd", L"pf", L"fxp", L"idx", L"key"
    };
    return s;
}

const wchar_t* kBlacklistQuery =
    L"exe;dll;sys;msi;msp;com;scr;drv;bat;cmd;ps1;vbs;js;jse;wsf;c;cpp;cxx;h;hpp;hh;cs;"
    L"java;jar;py;rb;go;rs;ts;jsx;tsx;html;htm;css;scss;less;xml;json;yml;yaml;toml;ini;"
    L"cfg;conf;reg;inf;manifest;props;targets;doc;docx;xls;xlsx;ppt;pptx;pdf;epub;mobi;"
    L"rtf;odt;ods;odp;md;tex;txt;jpg;jpeg;png;gif;bmp;tif;tiff;webp;ico;svg;psd;ai;heic;"
    L"avif;raw;mp3;wav;flac;ogg;m4a;aac;wma;mid;mp4;mkv;avi;mov;wmv;flv;webm;m4v;3gp;"
    L"mts;m2ts;zip;rar;7z;tar;gz;bz2;xz;z;iso;cab;msu;deb;rpm;apk;ipa;vhd;vhdx;img;db;"
    L"sqlite;sqlite3;mdb;accdb;dbf;mdf;ldf;pst;ost;bak;dmp;tmp;temp;cache;dat;pdb;obj;"
    L"lib;a;so;o;bin;lnk;url;nls;efi;mui;hlp;chm;ttf;otf;fon;wim;esd;pf;fxp;idx;key";

} // namespace

ExtClass ClassifyExt(const std::wstring& extLower) {
    std::wstring e = LowerAscii(extLower);
    if (e.empty()) return ExtClass::Unknown;
    if (LogExts().count(e)) return ExtClass::Log;
    if (NonLogExts().count(e)) return ExtClass::NotLog;
    return ExtClass::Unknown;
}

std::wstring BuildKnownLogQuery() {
    return L"ext:log;trace;etl;evtx;log1;log2";
}

std::wstring BuildNoExtQuery() {
    return L"ext:";
}

std::wstring BuildUnknownExtQuery() {
    std::wstring q = L"*.* !ext:";
    q += kBlacklistQuery;
    q += L" !ext:log;trace;etl;evtx;log1;log2";
    return q;
}

std::wstring BuildLargeQuery(uint64_t minSizeBytes) {
    wchar_t buf[64];
    // A bare size:N means "exactly N bytes" in Everything; >= is required to
    // return every file of any format at or above the threshold.
    swprintf_s(buf, L"size:>=%llu", (unsigned long long)minSizeBytes);
    return buf;
}

std::wstring BuildArchiveQuery() {
    return L"ext:zip;7z;rar;tar;gz;bz2;xz;zst;lz4;cab;iso;wim;arj;lzh;jar;apk;ipa;deb;rpm";
}

std::wstring BuildInstallerQuery() {
    return L"ext:msi;msp;msu;exe;appx;appxbundle;msix;msixbundle;dmg;pkg;deb;rpm;apk;ipa;iso";
}

ProbeOut ProbeFile(const std::wstring& path, uint64_t size, uint64_t maxProbeBytes) {
    ProbeOut out;
    if (size == 0) {
        out.kind = ProbeKind::Text; // empty file: treat as harmless text
        out.multiline = false;
        return out;
    }

    uint64_t readLen = std::min<uint64_t>(size, maxProbeBytes);
    if (readLen > 1024 * 1024) readLen = 1024 * 1024; // safety cap

    std::wstring openPath = AddLongPathPrefix(path);
    HANDLE h = CreateFileW(openPath.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        out.kind = ProbeKind::Unreadable;
        return out;
    }

    std::vector<unsigned char> buf((size_t)readLen);
    DWORD got = 0;
    if (!ReadFile(h, buf.data(), (DWORD)readLen, &got, nullptr) || got == 0) {
        CloseHandle(h);
        out.kind = ProbeKind::Unreadable;
        return out;
    }
    CloseHandle(h);

    bool hasNewline = false;
    for (DWORD i = 0; i < got; ++i) {
        if (buf[i] == '\n' || buf[i] == '\r') { hasNewline = true; break; }
    }
    out.multiline = hasNewline;

    // BOM detection
    if (got >= 3 && buf[0] == 0xEF && buf[1] == 0xBB && buf[2] == 0xBF) {
        out.kind = ProbeKind::Text;
        return out;
    }
    if (got >= 2 && ((buf[0] == 0xFF && buf[1] == 0xFE) || (buf[0] == 0xFE && buf[1] == 0xFF))) {
        out.kind = ProbeKind::Text;
        return out;
    }

    size_t n = std::min<size_t>((size_t)got, 4096);
    size_t good = 0;
    for (size_t i = 0; i < n; ++i) {
        unsigned char b = buf[i];
        if (b == 0) {
            out.kind = ProbeKind::Binary;
            return out;
        }
        if (b >= 0x20 && b <= 0x7E) ++good;       // printable ASCII
        else if (b == '\t' || b == '\n' || b == '\r') ++good; // whitespace
        else if (b >= 0x80) ++good;                // high bytes: GBK/UTF-8 payload
    }
    out.kind = (good * 100 >= n * 95) ? ProbeKind::Text : ProbeKind::Binary;
    return out;
}

bool PathExcluded(const std::wstring& path, const std::vector<std::wstring>& excludes) {
    for (const auto& e : excludes) {
        if (StartsWithIgnoreCase(path, e)) return true;
    }
    return false;
}

bool IsSystemExcludedPath(const std::wstring& path) {
    static const wchar_t* kSegments[] = {
        L"\\$recycle.bin\\", L"\\system volume information\\"
    };
    std::wstring lower = LowerAscii(path);
    for (const wchar_t* seg : kSegments) {
        if (lower.find(seg) != std::wstring::npos) return true;
    }
    return false;
}

bool IsUserPath(const std::wstring& path) {
    return StartsWithIgnoreCase(path, GetUserProfileDir());
}

bool MtimeWithinDays(const FILETIME& ft, int days) {
    if (days <= 0) return true;
    FILETIME now{};
    GetSystemTimeAsFileTime(&now);
    ULARGE_INTEGER a{}, b{};
    a.LowPart = ft.dwLowDateTime; a.HighPart = ft.dwHighDateTime;
    b.LowPart = now.dwLowDateTime; b.HighPart = now.dwHighDateTime;
    const uint64_t day100ns = 24ULL * 3600ULL * 10000000ULL;
    uint64_t diff = (b.QuadPart >= a.QuadPart) ? (b.QuadPart - a.QuadPart) : 0;
    return diff <= (uint64_t)days * day100ns;
}

uint64_t FileTimeToU64(const FILETIME& ft) {
    return ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
}
