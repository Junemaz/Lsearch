#include "test_util.h"

#include "core/search.h"
#include "dbus/logic.h"

#include <string>
#include <utility>
#include <vector>

using namespace lsearch;
using namespace lsearch::dbusbridge;

namespace {
const StatVariant* find(const std::vector<StatVariant>& v, const std::string& key) {
  for (const auto& s : v)
    if (s.key == key) return &s;
  return nullptr;
}
}  // namespace

TEST(dbus_make_search_args_valid) {
  mcp::SearchArgs sa;
  std::string err;
  CHECK(makeSearchArgs("report", 20, 0, "name", "any", sa, err));
  CHECK_EQ(sa.query, std::string("report"));
  CHECK_EQ(sa.limit, (size_t)20);
  CHECK_EQ(sa.offset, (size_t)0);
  CHECK(sa.sort == SortKey::Name);
  CHECK(sa.kind == mcp::Kind::Any);
  CHECK(err.empty());

  mcp::SearchArgs sa2;
  CHECK(makeSearchArgs("*.log", 5, 3, "mtime", "dirs", sa2, err));
  CHECK(sa2.sort == SortKey::Mtime);
  CHECK(sa2.kind == mcp::Kind::Dirs);
  CHECK_EQ(sa2.offset, (size_t)3);
}

TEST(dbus_make_search_args_whitelist) {
  mcp::SearchArgs sa;
  std::string err;
  CHECK(makeSearchArgs("q", 20, 0, "path", "files", sa, err));
  CHECK(sa.sort == SortKey::Path);
  CHECK(sa.kind == mcp::Kind::Files);

  CHECK(!makeSearchArgs("q", 20, 0, "bogus", "any", sa, err));
  CHECK(!err.empty());
  CHECK(!makeSearchArgs("q", 20, 0, "name", "bogus", sa, err));
  CHECK(!err.empty());
}

TEST(dbus_make_search_args_control_chars) {
  mcp::SearchArgs sa;
  std::string err;
  // 换行是最关键的注入向量：socket 是行协议。
  CHECK(!makeSearchArgs("x\nshutdown", 20, 0, "name", "any", sa, err));
  CHECK(!err.empty());
  CHECK(!makeSearchArgs("a\tb", 20, 0, "name", "any", sa, err));
  CHECK(!makeSearchArgs("a\rb", 20, 0, "name", "any", sa, err));
  CHECK(!makeSearchArgs(std::string("a\0b", 3), 20, 0, "name", "any", sa, err));
  // 含空格与 CJK 的合法查询仍通过
  CHECK(makeSearchArgs("年度 report", 20, 0, "name", "any", sa, err));
}

TEST(dbus_make_search_args_limit_offset_clamp) {
  mcp::SearchArgs sa;
  std::string err;
  CHECK(makeSearchArgs("q", 0, 0, "name", "any", sa, err));
  CHECK_EQ(sa.limit, (size_t)1);
  CHECK(makeSearchArgs("q", 1000000, 0, "name", "any", sa, err));
  CHECK_EQ(sa.limit, (size_t)200);
  CHECK(makeSearchArgs("q", 200, 50000, "name", "any", sa, err));
  CHECK_EQ(sa.offset, (size_t)50000);
  CHECK_EQ(mcp::overFetch(sa.offset, sa.limit), Index::kDefaultCandidateCap);
}

TEST(dbus_make_search_args_empty_and_regex) {
  mcp::SearchArgs sa;
  std::string err;
  CHECK(!makeSearchArgs("", 20, 0, "name", "any", sa, err));
  CHECK(!makeSearchArgs("   ", 20, 0, "name", "any", sa, err));
  CHECK(!makeSearchArgs("re:", 20, 0, "name", "any", sa, err));
  CHECK(!makeSearchArgs("re:[", 20, 0, "name", "any", sa, err));
  CHECK(makeSearchArgs("re:^AnnualReport\\.txt$", 20, 0, "name", "any", sa, err));
}

TEST(dbus_map_stats_types) {
  std::vector<std::pair<std::string, std::string>> kv = {
      {"files", "7"},   {"dirs", "2"},       {"size", "123456789012"},
      {"uptime", "42"}, {"roots", "/home/x"}, {"rebuilding", "1"},
      {"scan_files", "3"}, {"scan_dirs", "4"}};
  std::vector<StatVariant> v = mapStats(kv);
  CHECK_EQ(v.size(), (size_t)8);

  const StatVariant* roots = find(v, "roots");
  CHECK(roots && roots->type == StatVariant::Type::Str);
  CHECK_EQ(roots->str, std::string("/home/x"));

  const StatVariant* files = find(v, "files");
  CHECK(files && files->type == StatVariant::Type::U32);
  CHECK_EQ(files->u32, (uint32_t)7);
  const StatVariant* dirs = find(v, "dirs");
  CHECK(dirs && dirs->type == StatVariant::Type::U32);
  CHECK_EQ(dirs->u32, (uint32_t)2);
  const StatVariant* sf = find(v, "scan_files");
  CHECK(sf && sf->type == StatVariant::Type::U32);
  CHECK_EQ(sf->u32, (uint32_t)3);
  const StatVariant* sd = find(v, "scan_dirs");
  CHECK(sd && sd->type == StatVariant::Type::U32);
  CHECK_EQ(sd->u32, (uint32_t)4);

  const StatVariant* size = find(v, "size");
  CHECK(size && size->type == StatVariant::Type::U64);
  CHECK_EQ(size->u64, (uint64_t)123456789012ULL);
  const StatVariant* uptime = find(v, "uptime");
  CHECK(uptime && uptime->type == StatVariant::Type::U64);
  CHECK_EQ(uptime->u64, (uint64_t)42);

  const StatVariant* rb = find(v, "rebuilding");
  CHECK(rb && rb->type == StatVariant::Type::Bool);
  CHECK(rb->boolean);
}

TEST(dbus_map_stats_missing_keys) {
  std::vector<std::pair<std::string, std::string>> none;
  std::vector<StatVariant> v = mapStats(none);
  CHECK_EQ(v.size(), (size_t)8);
  const StatVariant* files = find(v, "files");
  CHECK(files && files->u32 == 0);
  const StatVariant* roots = find(v, "roots");
  CHECK(roots && roots->str.empty());
  const StatVariant* rb = find(v, "rebuilding");
  CHECK(rb && !rb->boolean);

  std::vector<std::pair<std::string, std::string>> off = {{"rebuilding", "0"}};
  CHECK(!find(mapStats(off), "rebuilding")->boolean);
}

TEST(dbus_rebuild_throttle) {
  RebuildThrottle t(5);
  CHECK(t.ready(1000));
  t.recordSuccess(1000);
  CHECK(!t.ready(1000));
  CHECK(!t.ready(1004));
  CHECK(t.ready(1005));
  CHECK(t.ready(1010));
  t.recordSuccess(2000);
  CHECK(!t.ready(2004));
  CHECK(t.ready(2005));

  // 冷启动窗口为 0：立即再次就绪
  RebuildThrottle z(0);
  CHECK(z.ready(0));
  z.recordSuccess(0);
  CHECK(z.ready(0));
}
