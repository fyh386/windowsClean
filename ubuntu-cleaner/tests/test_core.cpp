#include "filelist.h"
#include "probe.h"
#include "util.h"

#include <fcntl.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>

static int g_failures = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            ++g_failures;                                                    \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);           \
        }                                                                    \
    } while (0)

static std::string TempFile(const void* data, size_t size) {
    char tmpl[] = "/tmp/ubc-test-XXXXXX";
    int fd = mkstemp(tmpl);
    if (fd < 0) return "";
    if (size) write(fd, data, size);
    close(fd);
    return tmpl;
}

static void TestClassify() {
    CHECK(ClassifyExt("log") == ExtClass::Log);
    CHECK(ClassifyExt("LOG") == ExtClass::Log);
    CHECK(ClassifyExt("evtx") == ExtClass::Log);
    CHECK(ClassifyExt("exe") == ExtClass::NotLog);
    CHECK(ClassifyExt("zip") == ExtClass::NotLog);
    CHECK(ClassifyExt("pdf") == ExtClass::NotLog);
    CHECK(ClassifyExt("abc") == ExtClass::Unknown);
    CHECK(ClassifyExt("") == ExtClass::Unknown);
    CHECK(ExtOf("/a/b/file.LOG") == "log");
    CHECK(ExtOf("/a/b/noext") == "");
    CHECK(IsArchiveExt("zip"));
    CHECK(IsInstallerExt("deb"));
}

static void TestProbe() {
    const char utf8[] = "2026-08-28 10:00 INFO start\n2026-08-28 10:01 INFO done\n";
    std::string f1 = TempFile(utf8, sizeof(utf8) - 1);
    ProbeOut p1 = ProbeFile(f1, sizeof(utf8) - 1, 65536);
    CHECK(p1.kind == ProbeKind::Text);
    CHECK(p1.multiline);
    unlink(f1.c_str());

    // UTF-16LE with BOM
    std::vector<unsigned char> u16 = { 0xFF, 0xFE };
    const wchar_t w[] = L"hello\r\nworld\r\n";
    for (size_t i = 0; i < sizeof(w) - sizeof(wchar_t); ++i) {
        u16.push_back(((const unsigned char*)w)[i]);
    }
    std::string f2 = TempFile(u16.data(), u16.size());
    ProbeOut p2 = ProbeFile(f2, u16.size(), 65536);
    CHECK(p2.kind == ProbeKind::Text);
    CHECK(p2.multiline);
    unlink(f2.c_str());

    // binary with NUL
    unsigned char bin[512];
    for (int i = 0; i < 512; ++i) bin[i] = (unsigned char)(i % 251);
    bin[100] = 0;
    std::string f3 = TempFile(bin, sizeof(bin));
    ProbeOut p3 = ProbeFile(f3, sizeof(bin), 65536);
    CHECK(p3.kind == ProbeKind::Binary);
    unlink(f3.c_str());

    // empty file -> text
    std::string f4 = TempFile("", 0);
    ProbeOut p4 = ProbeFile(f4, 0, 65536);
    CHECK(p4.kind == ProbeKind::Text);
    unlink(f4.c_str());
}

static void TestSort() {
    FileVec items;
    FileItem a; a.path = "/b.log"; a.size = 10;
    FileItem b; b.path = "/a.txt"; b.size = 200;
    FileItem c; c.path = "/z.log"; c.size = 100;
    items.push_back(a); items.push_back(b); items.push_back(c);
    std::vector<unsigned> order;
    BuildSortedOrder(items, order, 3, false);
    CHECK(order.size() == 3);
    CHECK(items[order[0]].size == 200);
    CHECK(items[order[1]].size == 100);
    CHECK(items[order[2]].size == 10);
    BuildSortedOrder(items, order, 2, true);
    CHECK(items[order[0]].path == "/a.txt");
}

static void TestUtil() {
    CHECK(FormatBytes(1024) == "1.0 KB");
    CHECK(FileNameFromPath("/a/b/c.txt") == "c.txt");
    CHECK(DirFromPath("/a/b/c.txt") == "/a/b");
    CHECK(LowerAscii("AbC") == "abc");
    std::string p = "/tmp/ubc-ini-test.ini";
    SetIniValue(p, "a", "1");
    SetIniValue(p, "b", "hello world");
    SetIniValue(p, "a", "2");
    CHECK(GetIniValue(p, "a") == "2");
    CHECK(GetIniValue(p, "b") == "hello world");
    unlink(p.c_str());
}

int main() {
    TestClassify();
    TestProbe();
    TestSort();
    TestUtil();
    if (g_failures == 0) {
        printf("ALL TESTS PASSED\n");
        return 0;
    }
    printf("%d TEST(S) FAILED\n", g_failures);
    return 1;
}
