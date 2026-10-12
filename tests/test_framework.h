#pragma once

// Framework test tối giản, tự viết (không dùng gtest) — khớp với nguyên tắc
// "không dùng thư viện có sẵn" của dự án (mục 4 của kế hoạch), chỉ áp dụng
// cho phần giao thức/strategy, không áp dụng cho web UI hay test framework.
//
// Cách dùng trong 1 file test:
//   #include "test_framework.h"
//   void testFoo() { CHECK(1 + 1 == 2); }
//   int main() {
//       RUN(testFoo);
//       return TEST_REPORT();
//   }

#include <iostream>
#include <sstream>
#include <string>

namespace testutil {

inline int g_checks = 0;
inline int g_failures = 0;

inline void recordFailure(const std::string& expr, const char* file, int line) {
    ++g_failures;
    std::cerr << "  FAIL: " << expr << " (" << file << ":" << line << ")" << std::endl;
}

}  // namespace testutil

#define CHECK(cond)                                                       \
    do {                                                                  \
        ++testutil::g_checks;                                             \
        if (!(cond)) {                                                    \
            testutil::recordFailure(#cond, __FILE__, __LINE__);           \
        }                                                                 \
    } while (false)

#define CHECK_EQ(actual, expected)                                        \
    do {                                                                  \
        ++testutil::g_checks;                                             \
        const auto actualValue = (actual);                                \
        const auto expectedValue = (expected);                            \
        if (!(actualValue == expectedValue)) {                            \
            std::ostringstream oss;                                       \
            oss << #actual << " == " << #expected                         \
                << " (got " << actualValue << ", expected " << expectedValue << ")"; \
            testutil::recordFailure(oss.str(), __FILE__, __LINE__);       \
        }                                                                 \
    } while (false)

#define CHECK_THROWS(statement)                                           \
    do {                                                                  \
        ++testutil::g_checks;                                             \
        bool threw = false;                                               \
        try {                                                             \
            statement;                                                    \
        } catch (...) {                                                   \
            threw = true;                                                 \
        }                                                                 \
        if (!threw) {                                                     \
            testutil::recordFailure(                                      \
                std::string(#statement) + " was expected to throw",       \
                __FILE__, __LINE__);                                      \
        }                                                                 \
    } while (false)

#define RUN(testFn)                                                        \
    do {                                                                  \
        std::cout << "-- " << #testFn << std::endl;                       \
        testFn();                                                         \
    } while (false)

#define TEST_REPORT()                                                      \
    (std::cout << (testutil::g_failures == 0 ? "ALL PASSED" : "FAILED")    \
                << " (" << (testutil::g_checks - testutil::g_failures)     \
                << "/" << testutil::g_checks << " checks)" << std::endl,  \
     testutil::g_failures == 0 ? 0 : 1)
