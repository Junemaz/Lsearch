#include "test_util.h"

#include "core/util.h"

using namespace lsearch;

TEST(valid_config_path_accepts) {
  std::string why = "x";
  CHECK(validConfigPath("/data/a", why));
  CHECK(why.empty());
  CHECK(validConfigPath("relative/path", why));
  CHECK(validConfigPath("_", why));  // 哨兵由调用方解释，helper 本身接受
  CHECK(validConfigPath(std::string(4096, 'a'), why));
}

TEST(valid_config_path_rejects) {
  std::string why;
  CHECK(!validConfigPath("", why));
  CHECK(!why.empty());
  CHECK(!validConfigPath("a,b", why));
  CHECK(!validConfigPath("/data/a,b", why));
  CHECK(!validConfigPath("a\tb", why));
  CHECK(!validConfigPath("a\nb", why));
  CHECK(!validConfigPath("a\rb", why));
  CHECK(!validConfigPath(std::string(1, '\x1f'), why));
  CHECK(!validConfigPath(std::string(1, '\x7f'), why));
  CHECK(!validConfigPath(std::string(4097, 'a'), why));
  CHECK(!validConfigPath(" a", why));
  CHECK(!validConfigPath("a ", why));
  CHECK(!validConfigPath("  ", why));
  CHECK(!validConfigPath("\ta", why));
  CHECK(!validConfigPath("a\t", why));
}

TEST(parse_limit_accepts) {
  size_t v = 12345;
  CHECK(parseLimit("0", v));
  CHECK_EQ(v, (size_t)0);
  CHECK(parseLimit("200", v));
  CHECK_EQ(v, (size_t)200);
  CHECK(parseLimit("0200", v));
  CHECK_EQ(v, (size_t)200);
  CHECK(parseLimit("00", v));
  CHECK_EQ(v, (size_t)0);
  CHECK(parseLimit("1048576", v));
  CHECK_EQ(v, kLimitMax);
}

TEST(parse_limit_rejects) {
  size_t v = 999;
  CHECK(!parseLimit("", v));
  CHECK(!parseLimit("1048577", v));
  CHECK(!parseLimit("-1", v));
  CHECK(!parseLimit("abc", v));
  CHECK(!parseLimit("+1", v));
  CHECK(!parseLimit(" 1", v));
  CHECK(!parseLimit("1 ", v));
  CHECK(!parseLimit("1.0", v));
  CHECK(!parseLimit("0x10", v));
  CHECK(!parseLimit("99999999999999999999", v));
  CHECK_EQ(v, (size_t)999);  // 失败时不改写 out
}
