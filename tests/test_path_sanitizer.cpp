#include "test_framework.h"

#include "core/path_sanitizer.h"

using namespace p2p;

void testAcceptsNormalPaths() {
    CHECK(PathSanitizer::isSafeRelativePath("file.txt"));
    CHECK(PathSanitizer::isSafeRelativePath("sub/dir/file.txt"));
    CHECK_EQ(PathSanitizer::normalize("sub/dir/file.txt"), std::string("sub/dir/file.txt"));
}

void testRejectsEmptyPath() {
    CHECK(!PathSanitizer::isSafeRelativePath(""));
    CHECK_EQ(PathSanitizer::normalize(""), std::string(""));
}

void testRejectsParentDirectoryTraversal() {
    // ".." trong path có thể ghi file ra ngoài thư mục tải (mục 5.1).
    CHECK(!PathSanitizer::isSafeRelativePath(".."));
    CHECK(!PathSanitizer::isSafeRelativePath("../secret.txt"));
    CHECK(!PathSanitizer::isSafeRelativePath("a/../../b"));
    CHECK(!PathSanitizer::isSafeRelativePath("a/.."));
    CHECK(!PathSanitizer::isSafeRelativePath("."));

    CHECK_EQ(PathSanitizer::normalize("../secret.txt"), std::string(""));
    CHECK_EQ(PathSanitizer::normalize("a/../../b"), std::string(""));
}

void testRejectsAbsolutePaths() {
    CHECK(!PathSanitizer::isSafeRelativePath("/etc/passwd"));
    CHECK(!PathSanitizer::isSafeRelativePath("a/"));
    CHECK_EQ(PathSanitizer::normalize("/etc/passwd"), std::string(""));
}

void testRejectsWindowsDriveAndBackslash() {
    CHECK(!PathSanitizer::isSafeRelativePath("C:/windows/system32"));
    CHECK(!PathSanitizer::isSafeRelativePath("a\\b"));
    CHECK_EQ(PathSanitizer::normalize("C:/windows/system32"), std::string(""));
}

void testRejectsNulByte() {
    const std::string withNul = std::string("a") + '\0' + "b";
    CHECK(!PathSanitizer::isSafeRelativePath(withNul));
}

void testRejectsReservedWindowsNames() {
    CHECK(!PathSanitizer::isSafeRelativePath("CON"));
    CHECK(!PathSanitizer::isSafeRelativePath("con"));  // không phân biệt hoa/thường
    CHECK(!PathSanitizer::isSafeRelativePath("NUL"));
    CHECK(!PathSanitizer::isSafeRelativePath("a/COM1"));
    CHECK(!PathSanitizer::isSafeRelativePath("LPT9"));
    // Tên trông giống nhưng không trùng không bị chặn.
    CHECK(PathSanitizer::isSafeRelativePath("CONtent.txt"));
}

void testRejectsDeviceNamesWithExtension() {
    for (const char* bad : {"CON.txt", "nul.x", "Aux", "aux.tar.gz", "PRN.log", "com1.txt", "COM9.a",
                            "LPT1.txt", "lpt9", "dir/CON.txt", "dir/nul.x/file", "Con.TXT"}) {
        CHECK(!PathSanitizer::isSafeRelativePath(bad));
    }
    // Tên chỉ bắt đầu giống thiết bị thì vẫn hợp lệ.
    for (const char* good : {"CONtent.txt", "console", "COM10", "COM0.txt", "LPT.txt", "aux1", "nulled.txt",
                             "a.con", "prn_file"}) {
        CHECK(PathSanitizer::isSafeRelativePath(good));
    }
}

void testRejectsTrailingDotOrSpace() {
    for (const char* bad : {"file.", "file ", "dir./file", "dir /file", "a/b.", "a/b ", "name..", "x. "}) {
        CHECK(!PathSanitizer::isSafeRelativePath(bad));
    }
    CHECK(PathSanitizer::isSafeRelativePath("a b/c d.txt"));
    CHECK(PathSanitizer::isSafeRelativePath(".hidden"));
}

void testRejectsForbiddenCharactersAndControls() {
    for (const char* bad : {"a<b", "a>b", "a\"b", "a|b", "a?b", "a*b", "dir/fi*le", "dir/a?b.txt"}) {
        CHECK(!PathSanitizer::isSafeRelativePath(bad));
    }
    for (int code = 1; code < 0x20; ++code) {
        const std::string withControl = std::string("a") + static_cast<char>(code) + "b";
        CHECK(!PathSanitizer::isSafeRelativePath(withControl));
    }
    CHECK(!PathSanitizer::isSafeRelativePath(std::string("dir/a") + '\n' + "b"));
    CHECK(PathSanitizer::isSafeRelativePath("a-b_c(1)[2]{3}#4@5%6&7+8=9,10;11'12~13.txt"));
}

void testNormalizeAppliesSameSegmentRules() {
    CHECK_EQ(PathSanitizer::normalize("a/CON.txt"), std::string(""));
    CHECK_EQ(PathSanitizer::normalize("a/b?"), std::string(""));
    CHECK_EQ(PathSanitizer::normalize("a/b."), std::string(""));
    CHECK_EQ(PathSanitizer::normalize("a/ok.txt"), std::string("a/ok.txt"));
}

void testNormalizeDropsDotSegments() {
    CHECK_EQ(PathSanitizer::normalize("./a/./b"), std::string("a/b"));
}

int main() {
    RUN(testAcceptsNormalPaths);
    RUN(testRejectsEmptyPath);
    RUN(testRejectsParentDirectoryTraversal);
    RUN(testRejectsAbsolutePaths);
    RUN(testRejectsWindowsDriveAndBackslash);
    RUN(testRejectsNulByte);
    RUN(testRejectsReservedWindowsNames);
    RUN(testRejectsDeviceNamesWithExtension);
    RUN(testRejectsTrailingDotOrSpace);
    RUN(testRejectsForbiddenCharactersAndControls);
    RUN(testNormalizeAppliesSameSegmentRules);
    RUN(testNormalizeDropsDotSegments);
    return TEST_REPORT();
}
