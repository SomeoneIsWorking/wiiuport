#pragma once

#include <cmath>
#include <cstdio>
#include <sstream>
#include <string>

// A deliberately small harness rather than a test framework dependency.
//
// It exists to report denominators and to fail the process: a run that
// checked nothing must not be reportable as a run that passed. Every check
// counts, whether it passed or not, and the totals are printed unconditionally
// including the zero case.
namespace check {

inline int g_checks = 0;
inline int g_failures = 0;

inline void isTrue(bool condition, const std::string& what) {
    g_checks++;
    if (!condition) {
        g_failures++;
        std::printf("  FAIL %s\n", what.c_str());
    }
}

inline void near(float actual, float expected, float tolerance, const std::string& what) {
    bool ok = std::abs(actual - expected) <= tolerance;
    if (!ok) {
        std::printf("  FAIL %s: %.9g is not within %.3g of %.9g\n", what.c_str(),
                    static_cast<double>(actual), static_cast<double>(tolerance),
                    static_cast<double>(expected));
        g_checks++;
        g_failures++;
        return;
    }
    isTrue(true, what);
}

// Prints both operands, because "FAIL the address is kept" without the two
// values sends the reader back to the debugger to learn what it already knew.
template <typename T, typename U>
inline void equal(const T& actual, const U& expected, const std::string& what) {
    g_checks++;
    if (actual == expected) {
        return;
    }
    g_failures++;
    std::ostringstream message;
    message << "  FAIL " << what << ": " << actual << " != " << expected << "\n";
    std::fputs(message.str().c_str(), stdout);
}

} // namespace check
