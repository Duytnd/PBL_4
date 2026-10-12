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
    RUN(testNormalizeDropsDotSegments);
    return TEST_REPORT();
}
