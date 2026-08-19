#pragma once
// 极简单头测试框架：TEST(name) 注册，CHECK/CHECK_EQ 断言
#include <cstdio>
#include <string>
#include <vector>

namespace ltest {
inline int checks = 0;
inline int failures = 0;

struct TestCase {
  const char* name;
  void (*fn)();
};
inline std::vector<TestCase>& registry() {
  static std::vector<TestCase> r;
  return r;
}
struct Reg {
  Reg(const char* n, void (*f)()) { registry().push_back({n, f}); }
};
}  // namespace ltest

#define TEST(name)                                                     \
  static void test_##name();                                           \
  static ::ltest::Reg reg_##name(#name, test_##name);                  \
  static void test_##name()

#define CHECK(cond)                                                           \
  do {                                                                        \
    if (cond) {                                                               \
      ++::ltest::checks;                                                      \
    } else {                                                                  \
      ++::ltest::failures;                                                    \
      fprintf(stderr, "  [FAIL] %s:%d  %s\n", __FILE__, __LINE__, #cond);     \
    }                                                                         \
  } while (0)

#define CHECK_EQ(a, b)                                                        \
  do {                                                                        \
    auto _a = (a);                                                            \
    auto _b = (b);                                                            \
    if (_a == _b) {                                                           \
      ++::ltest::checks;                                                      \
    } else {                                                                  \
      ++::ltest::failures;                                                    \
      fprintf(stderr, "  [FAIL] %s:%d  %s == %s\n", __FILE__, __LINE__,       \
              #a, #b);                                                        \
    }                                                                         \
  } while (0)
