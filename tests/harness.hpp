// SPDX-License-Identifier: MIT
//
// Minimal test harness: a registration list, two assertion macros and a runner.
// Deliberately dependency-free so `ctest` works on a machine that only has a compiler and
// libav installed (no gtest download, no package manager round trip).
#ifndef AVPIPE_TESTS_HARNESS_HPP
#define AVPIPE_TESTS_HARNESS_HPP

#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <type_traits>
#include <vector>

namespace mini {

struct TestCase {
  std::string name;
  std::function<void()> body;
};

inline std::vector<TestCase>& registry() {
  static std::vector<TestCase> tests;
  return tests;
}

inline int& failure_count() {
  static int failures = 0;
  return failures;
}

inline std::string& current_test() {
  static std::string name;
  return name;
}

struct Registrar {
  Registrar(const char* name, std::function<void()> body) {
    registry().push_back(TestCase{name, std::move(body)});
  }
};

inline void report(const char* file, int line, const std::string& expression,
                   const std::string& detail) {
  ++failure_count();
  std::cerr << "  FAIL " << current_test() << " at " << file << ":" << line << ": " << expression;
  if (!detail.empty()) {
    std::cerr << " (" << detail << ")";
  }
  std::cerr << "\n";
}

/// Streams anything the type supports; `std::to_string` only covers arithmetic types and would
/// make `CHECK_EQ` unusable on std::string.
template <class T>
std::string stringify(const T& value) {
  std::ostringstream os;
  os << value;
  return os.str();
}

inline int run_all() {
  int run = 0;
  for (const TestCase& test : registry()) {
    current_test() = test.name;
    const int before = failure_count();
    test.body();
    ++run;
    std::cout << (failure_count() == before ? "[  OK  ] " : "[ FAIL ] ") << test.name << "\n";
  }
  std::cout << run << " test(s), " << failure_count() << " failure(s)\n";
  return failure_count() == 0 ? 0 : 1;
}

}  // namespace mini

#define TEST_CASE(name)                                                     \
  static void name();                                                       \
  static ::mini::Registrar name##_registrar(#name, name);                   \
  static void name()

#define CHECK(condition)                                                    \
  do {                                                                      \
    if (!(condition)) {                                                     \
      ::mini::report(__FILE__, __LINE__, #condition, "");                   \
    }                                                                       \
  } while (false)

#define CHECK_EQ(actual, expected)                                          \
  do {                                                                      \
    const auto& a_ = (actual);                                              \
    const auto& e_ = (expected);                                            \
    if (!(a_ == e_)) {                                                      \
      ::mini::report(__FILE__, __LINE__, #actual " == " #expected,          \
                     ::mini::stringify(a_) + " vs " + ::mini::stringify(e_)); \
    }                                                                       \
  } while (false)

#define TEST_MAIN()                                                         \
  int main() { return ::mini::run_all(); }

#endif  // AVPIPE_TESTS_HARNESS_HPP
