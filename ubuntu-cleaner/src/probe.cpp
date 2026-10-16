#include "probe.h"

#include "util.h"

#include <algorithm>
#include <cstdio>
#include <set>
#include <vector>

namespace {

const std::set<std::string>& LogExts() {
    static const std::set<std::string> s = { "log", "trace", "etl", "evtx", "log1", "log2" };
    return s;
}

const std::set<std::string>& NonLogExts() {
    static const std::set<std::string> s = {
        // executables / libraries / archives
        "exe", "dll", "sys", "msi", "msp", "msu", "bin", "so", "a", "o", "ko",
        "zip", "7z", "rar", "tar", "gz", "bz2", "xz", "zst", "lz4", "cab", "iso",
        "wim", "arj", "lzh", "jar", "apk", "ipa", "deb", "rpm", "dmg", "pkg",
        // images
        "jpg", "jpeg", "png", "gif", "bmp", "webp", "svg", "ico", "tif", "tiff", "heic",
        // media
        "mp4", "mkv", "avi", "mov", "wmv", "flv", "webm", "mp3", "wav", "flac",
        "aac", "ogg", "opus", "m4a", "wma",
        // documents
        "doc", "docx", "xls", "xlsx", "ppt", "pptx", "pdf", "odt", "ods", "odp",
        "rtf", "epub", "mobi",
        // source / config / data
        "c", "cpp", "h", "hpp", "java", "py", "cs", "go", "rs", "js", "ts", "css",
        "html", "htm", "xml", "json", "yml", "yaml", "ini", "cfg", "conf", "toml",
        "sql", "db", "sqlite", "mdf", "ldf", "tmp", "cache", "dat",
        // fonts
        "ttf", "otf", "woff", "woff2",
        // linux executables
        "elf", "run", "AppImage",
    };
    return s;
}

const std::set<std::string>& ArchiveExts() {
    static const std::set<std::string> s = {
        "zip", "7z", "rar", "tar", "gz", "bz2", "xz", "zst", "lz4", "cab", "iso",
        "wim", "arj", "lzh", "jar", "apk", "ipa", "deb", "rpm",
    };
    return s;
}

const std::set<std::string>& InstallerExts() {
    static const std::set<std::string> s = {
        "msi", "msp", "msu", "exe", "appx", "appxbundle", "msix", "msixbundle",
        "dmg", "pkg", "deb", "rpm", "apk", "ipa", "iso", "run", "AppImage",
    };
    return s;
}

} // namespace

ExtClass ClassifyExt(const std::string& extLower) {
    if (extLower.empty()) return ExtClass::Unknown;
    if (LogExts().count(extLower)) return ExtClass::Log;
    if (NonLogExts().count(extLower)) return ExtClass::NotLog;
    return ExtClass::Unknown;
}

bool IsArchiveExt(const std::string& extLower) {
    return ArchiveExts().count(extLower) != 0;
}

bool IsInstallerExt(const std::string& extLower) {
    return InstallerExts().count(extLower) != 0;
}

std::string ExtOf(const std::string& path) {
    std::string name = FileNameFromPath(path);
    size_t dot = name.find_last_of('.');
    if (dot == std::string::npos || dot + 1 >= name.size()) return "";
    return LowerAscii(name.substr(dot + 1));
}

ProbeOut ProbeFile(const std::string& path, uint64_t size, uint64_t maxProbeBytes) {
    ProbeOut out;
    if (size == 0) {
        out.kind = ProbeKind::Text;
        out.multiline = false;
        return out;
    }

    uint64_t readLen = std::min<uint64_t>(size, maxProbeBytes);
    if (readLen > 1024 * 1024) readLen = 1024 * 1024;

    FILE* f = fopen(path.c_str(), "rb");
    if (!f) {
        out.kind = ProbeKind::Unreadable;
        return out;
    }
    std::vector<unsigned char> buf((size_t)readLen);
    size_t got = fread(buf.data(), 1, (size_t)readLen, f);
    fclose(f);
    if (got == 0) {
        out.kind = ProbeKind::Unreadable;
        return out;
    }

    // UTF-16LE/BE BOM -> text directly.
    if (got >= 2 && ((buf[0] == 0xFF && buf[1] == 0xFE) ||
                     (buf[0] == 0xFE && buf[1] == 0xFF))) {
        out.kind = ProbeKind::Text;
        out.multiline = false;
        for (size_t i = 2; i + 1 < got; i += 2) {
            if (buf[i] == '\n' || buf[i] == '\r') { out.multiline = true; break; }
        }
        return out;
    }

    bool hasNewline = false;
    size_t good = 0;
    for (size_t i = 0; i < got; ++i) {
        unsigned char b = buf[i];
        if (b == '\n' || b == '\r') hasNewline = true;
        if ((b >= 0x20 && b <= 0x7E) || b == '\t' || b == '\n' || b == '\r' || b >= 0x80) ++good;
    }
    out.kind = (good * 100 >= got * 95) ? ProbeKind::Text : ProbeKind::Binary;
    out.multiline = hasNewline;
    return out;
}
