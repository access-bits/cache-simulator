#pragma once

// A deliberately tiny test harness.
//
// There is no GoogleTest or Catch2 here on purpose: the simulator itself has
// no third-party dependency beyond one header-only hash map, and adding a
// test framework that has to be fetched at configure time would make the
// build depend on the network. These tests need assertions, named cases, and
// a non-zero exit code on failure, which is about forty lines.

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <string>
#include <type_traits>
#include <vector>

namespace cachesim::test {

inline int g_failures = 0;
inline int g_checks = 0;
inline int g_case_failures_at_start = 0;

inline void beginCase(const std::string& name) {
  g_case_failures_at_start = g_failures;
  std::printf("  %-54s", name.c_str());
  std::fflush(stdout);
}

inline void endCase() {
  std::printf("%s\n", g_failures == g_case_failures_at_start ? "ok" : "FAILED");
}

// Each CHECK prints only on failure, so a passing run stays readable and a
// failing one points straight at the line.
inline void reportFailure(const char* file, int line, const std::string& detail) {
  ++g_failures;
  std::printf("\n    FAIL %s:%d\n      %s\n", file, line, detail.c_str());
}

// Renders anything printable, including pointers and strings, which
// std::to_string cannot.
template <typename T>
std::string describe(const T& value) {
  if constexpr (std::is_same_v<T, std::string>) {
    return "\"" + value + "\"";
  } else if constexpr (std::is_convertible_v<T, const char*>) {
    return std::string("\"") + value + "\"";
  } else {
    std::ostringstream out;
    out << value;
    return out.str();
  }
}

template <typename A, typename B>
void checkEqual(const A& actual, const B& expected, const char* expr, const char* file,
                int line) {
  ++g_checks;
  if (actual == expected) return;
  reportFailure(file, line,
                std::string(expr) + "\n      actual:   " + describe(actual) +
                    "\n      expected: " + describe(expected));
}

inline void checkTrue(bool value, const char* expr, const char* file, int line) {
  ++g_checks;
  if (value) return;
  reportFailure(file, line, std::string(expr) + " is false");
}

inline void checkNear(double actual, double expected, double tolerance, const char* expr,
                      const char* file, int line) {
  ++g_checks;
  if (std::fabs(actual - expected) <= tolerance) return;
  reportFailure(file, line,
                std::string(expr) + "\n      actual:   " + describe(actual) +
                    "\n      expected: " + describe(expected) + " +/- " +
                    describe(tolerance));
}

inline int summarize(const char* suite) {
  if (g_failures == 0) {
    std::printf("%s: %d checks passed\n", suite, g_checks);
    return 0;
  }
  std::printf("%s: %d of %d checks FAILED\n", suite, g_failures, g_checks);
  return 1;
}

}  // namespace cachesim::test

#define CHECK(expr) ::cachesim::test::checkTrue((expr), #expr, __FILE__, __LINE__)
#define CHECK_EQ(actual, expected) \
  ::cachesim::test::checkEqual((actual), (expected), #actual " == " #expected, __FILE__, __LINE__)
#define CHECK_NEAR(actual, expected, tol)                                               \
  ::cachesim::test::checkNear((actual), (expected), (tol), #actual " ~= " #expected, \
                              __FILE__, __LINE__)
#define TEST_CASE(name)                                                      \
  for (bool cachesim_test_once = (::cachesim::test::beginCase(name), true);  \
       cachesim_test_once;                                                   \
       cachesim_test_once = (::cachesim::test::endCase(), false))
