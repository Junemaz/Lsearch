#include "test_util.h"

#include "core/entry.h"
#include "core/search.h"
#include "mcp/json.h"
#include "mcp/protocol.h"

#include <limits>
#include <string>
#include <vector>

using namespace lsearch;
using namespace mcp;

static std::vector<SearchResult> pageSample() {
  std::vector<SearchResult> v;
  for (int i = 0; i < 5; ++i) {
    SearchResult r;
    r.entry.path = "/x/p" + std::to_string(i);
    r.entry.name = "n" + std::to_string(i);
    r.entry.size = i * 10;
    r.entry.mtime = 100 + i;
    r.entry.is_dir = (i % 2 == 0);
    v.push_back(r);
  }
  return v;
}

TEST(mcp_json_escape_roundtrip) {
  Json j = Json::str("a\"b\\c\nd\te");
  std::string d = j.dump();
  CHECK_EQ(d, std::string("\"a\\\"b\\\\c\\nd\\te\""));
  Json back;
  std::string err;
  CHECK(Json::parse(d, back, err));
  CHECK_EQ(back.asString(), std::string("a\"b\\c\nd\te"));

  // 控制字符 → \uXXXX
  Json ctl = Json::str(std::string("x\x01y"));
  CHECK(ctl.dump().find("\\u0001") != std::string::npos);

  // CJK 原样保留（合法 UTF-8 不转义）并可回读
  Json cjk = Json::str("报告");
  std::string cd = cjk.dump();
  CHECK(cd.find("\xE6\x8A\xA5\xE5\x91\x8A") != std::string::npos);
  Json cback;
  CHECK(Json::parse(cd, cback, err));
  CHECK_EQ(cback.asString(), std::string("报告"));
}

TEST(mcp_json_invalid_utf8_sanitized) {
  std::string bad = "ok\xFF\xFE";
  std::string d = Json::str(bad).dump();
  CHECK(d.find("\xFF") == std::string::npos);
  CHECK(d.find("\xEF\xBF\xBD") != std::string::npos);
  Json back;
  std::string err;
  CHECK(Json::parse(d, back, err));
  // 两个非法字节各替换为一个 U+FFFD
  std::string rep = "\xEF\xBF\xBD";
  size_t count = 0, pos = 0;
  while ((pos = back.asString().find(rep, pos)) != std::string::npos) {
    ++count;
    pos += rep.size();
  }
  CHECK_EQ(count, (size_t)2);
}

TEST(mcp_json_parse_errors) {
  Json out;
  std::string err;
  CHECK(!Json::parse("", out, err));
  CHECK(!Json::parse("{", out, err));
  CHECK(!Json::parse("{\"a\":}", out, err));
  CHECK(!Json::parse("[1,2", out, err));
  CHECK(!Json::parse("123 abc", out, err));
  CHECK(!Json::parse("tru", out, err));
  CHECK(!Json::parse("\"unterminated", out, err));

  CHECK(Json::parse("{\"a\":[1,2.5,true,null,\"x\"]}", out, err));
  CHECK(out.isObject());
  const Json* a = out.find("a");
  CHECK(a && a->isArray());
  CHECK_EQ(a->size(), (size_t)5);
  CHECK_EQ((*a).items()[0].asInt(), 1);
  CHECK_EQ((*a).items()[1].asNumber(), 2.5);
  CHECK((*a).items()[2].asBool());
  CHECK((*a).items()[3].isNull());
  CHECK_EQ((*a).items()[4].asString(), std::string("x"));
}

TEST(mcp_limit_clamp) {
  CHECK_EQ(clampLimit(0), (size_t)1);
  CHECK_EQ(clampLimit(-3), (size_t)1);
  CHECK_EQ(clampLimit(1), (size_t)1);
  CHECK_EQ(clampLimit(20), (size_t)20);
  CHECK_EQ(clampLimit(200), (size_t)200);
  CHECK_EQ(clampLimit(201), (size_t)200);
  CHECK_EQ(clampLimit(1000000), (size_t)200);
}

TEST(mcp_overfetch_arithmetic) {
  CHECK_EQ(overFetch(0, 20), (size_t)20);
  CHECK_EQ(overFetch(100, 100), (size_t)200);
  CHECK_EQ(overFetch(49000, 200), (size_t)49200);
  CHECK_EQ(overFetch(49900, 200), (size_t)50000);
  CHECK_EQ(overFetch(50000, 20), (size_t)50000);
  CHECK_EQ(overFetch(60000, 1), (size_t)50000);
}

TEST(mcp_paginate_slicing) {
  std::vector<SearchResult> f = pageSample();
  Page p0 = paginate(f, 0, 2);
  CHECK_EQ(p0.returned, (size_t)2);
  CHECK(p0.has_more);
  CHECK(!p0.truncated);
  CHECK_EQ(p0.items[0].entry.path, std::string("/x/p0"));
  CHECK_EQ(p0.items[1].entry.path, std::string("/x/p1"));

  Page p1 = paginate(f, 2, 2);
  CHECK_EQ(p1.returned, (size_t)2);
  CHECK(p1.has_more);
  CHECK_EQ(p1.items[0].entry.path, std::string("/x/p2"));
  // 页间不重叠
  for (const auto& a : p0.items)
    for (const auto& b : p1.items) CHECK(a.entry.path != b.entry.path);

  Page p2 = paginate(f, 4, 2);
  CHECK_EQ(p2.returned, (size_t)1);
  CHECK(!p2.has_more);
  CHECK_EQ(p2.items[0].entry.path, std::string("/x/p4"));

  Page whole = paginate(f, 0, 10);
  CHECK_EQ(whole.returned, (size_t)5);
  CHECK(!whole.has_more);
}

TEST(mcp_paginate_cap) {
  std::vector<SearchResult> f = pageSample();
  Page beyond = paginate(f, 50000, 20);
  CHECK_EQ(beyond.returned, (size_t)0);
  CHECK(!beyond.has_more);
  CHECK(beyond.truncated);

  Page edge = paginate(f, 49900, 200);
  CHECK_EQ(edge.returned, (size_t)0);
  CHECK(edge.truncated);
  CHECK(!edge.has_more);

  Page normal = paginate(f, 0, 20);
  CHECK(!normal.truncated);
}

TEST(mcp_cap_truncation) {
  std::vector<SearchResult> f = pageSample();
  Page small = paginate(f, 0, 2);
  CHECK(!small.truncated);
  applyCapTruncation(small, 2);
  CHECK(!small.truncated);

  Page capped = paginate(f, 0, 2);
  applyCapTruncation(capped, kMaxFetch);
  CHECK(capped.truncated);
  CHECK_EQ(capped.returned, (size_t)2);
}

TEST(mcp_parse_args) {
  Json a = Json::object();
  a.set("query", Json::str("x"));
  a.set("limit", Json::integer(0));
  a.set("offset", Json::integer(3));
  a.set("sort", Json::str("size"));
  a.set("kind", Json::str("dirs"));
  SearchArgs sa;
  std::string err;
  CHECK(parseSearchArgs(a, sa, err));
  CHECK_EQ(sa.limit, (size_t)1);  // 0 → 钳制为 1
  CHECK_EQ(sa.offset, (size_t)3);
  CHECK(sa.sort == SortKey::Size);
  CHECK(sa.kind == Kind::Dirs);

  SearchArgs def;
  Json only = Json::object();
  only.set("query", Json::str("hello"));
  CHECK(parseSearchArgs(only, def, err));
  CHECK_EQ(def.limit, kDefaultLimit);
  CHECK_EQ(def.offset, (size_t)0);
  CHECK(def.sort == SortKey::Name);
  CHECK(def.kind == Kind::Any);

  Json neg = Json::object();
  neg.set("query", Json::str("q"));
  neg.set("offset", Json::integer(-10));
  SearchArgs nsa;
  CHECK(parseSearchArgs(neg, nsa, err));
  CHECK_EQ(nsa.offset, (size_t)0);

  Json missing = Json::object();
  missing.set("limit", Json::integer(5));
  CHECK(!parseSearchArgs(missing, sa, err));

  Json empty = Json::object();
  empty.set("query", Json::str("   "));
  CHECK(!parseSearchArgs(empty, sa, err));

  Json badSort = Json::object();
  badSort.set("query", Json::str("q"));
  badSort.set("sort", Json::str("bogus"));
  CHECK(!parseSearchArgs(badSort, sa, err));

  Json badKind = Json::object();
  badKind.set("query", Json::str("q"));
  badKind.set("kind", Json::str("bogus"));
  CHECK(!parseSearchArgs(badKind, sa, err));

  Json badType = Json::object();
  badType.set("query", Json::str("q"));
  badType.set("limit", Json::str("20"));
  CHECK(!parseSearchArgs(badType, sa, err));

  Json extra = Json::object();
  extra.set("query", Json::str("q"));
  extra.set("under", Json::str("/tmp"));
  CHECK(!parseSearchArgs(extra, sa, err));

  Json notObj = Json::makeNull();
  CHECK(!parseSearchArgs(notObj, sa, err));
}

TEST(mcp_tool_mapping) {
  CHECK(toolFromName("search_files") == Tool::SearchFiles);
  CHECK(toolFromName("index_stats") == Tool::IndexStats);
  CHECK(toolFromName("shutdown") == Tool::Unknown);
  CHECK(toolFromName("rebuild") == Tool::Unknown);
  CHECK(toolFromName("set-paths") == Tool::Unknown);
  CHECK(toolFromName("add-path") == Tool::Unknown);
}

TEST(mcp_meta_validation) {
  Json none = Json::object();
  ModernMeta m = checkModernMeta(none);
  CHECK(!m.hasMeta);

  Json paramsNoMeta = Json::object();
  paramsNoMeta.set("params", Json::object());
  m = checkModernMeta(paramsNoMeta);
  CHECK(!m.hasMeta);

  Json partial = Json::object();
  Json p1 = Json::object();
  Json meta1 = Json::object();
  meta1.set("io.modelcontextprotocol/protocolVersion", Json::str(kModernVersion));
  p1.set("_meta", meta1);
  partial.set("params", p1);
  m = checkModernMeta(partial);
  CHECK(m.hasMeta);
  CHECK(!m.valid);  // 缺 clientCapabilities

  Json full = Json::object();
  Json p2 = Json::object();
  Json meta2 = Json::object();
  meta2.set("io.modelcontextprotocol/protocolVersion", Json::str(kModernVersion));
  meta2.set("io.modelcontextprotocol/clientCapabilities", Json::object());
  p2.set("_meta", meta2);
  full.set("params", p2);
  m = checkModernMeta(full);
  CHECK(m.hasMeta);
  CHECK(m.valid);
  CHECK_EQ(m.protocolVersion, std::string(kModernVersion));

  Json plain = Json::object();
  Json p3 = Json::object();
  Json meta3 = Json::object();
  meta3.set("io.modelcontextprotocol/protocolVersion", Json::str(kModernVersion));
  meta3.set("clientCapabilities", Json::object());
  p3.set("_meta", meta3);
  plain.set("params", p3);
  m = checkModernMeta(plain);
  CHECK(m.valid);
}

TEST(mcp_meta_legacy_tolerated) {
  // legacy 客户端 _meta 可携带 progressToken 等自定义键：不得视为非法 modern 请求。
  Json legacy = Json::object();
  Json p = Json::object();
  Json meta = Json::object();
  meta.set("progressToken", Json::str("tok1"));
  p.set("_meta", meta);
  legacy.set("params", p);
  ModernMeta m = checkModernMeta(legacy);
  CHECK(!m.hasMeta);
  CHECK(!m.valid);
  CHECK(m.error.empty());

  Json nonObject = Json::object();
  Json p2 = Json::object();
  p2.set("_meta", Json::str("x"));
  nonObject.set("params", p2);
  CHECK(!checkModernMeta(nonObject).hasMeta);
}

TEST(mcp_builders) {
  Json discover = discoverResult();
  CHECK_EQ(discover.find("resultType")->asString(), std::string("complete"));
  CHECK(discover.find("ttlMs") != nullptr);
  CHECK_EQ(discover.find("cacheScope")->asString(), std::string("public"));
  const Json* vers = discover.find("supportedVersions");
  CHECK(vers && vers->isArray());
  bool hasModern = false, hasLegacy = false;
  for (const auto& v : vers->items()) {
    if (v.asString() == kModernVersion) hasModern = true;
    if (v.asString() == kLegacyVersion) hasLegacy = true;
  }
  CHECK(hasModern);
  CHECK(hasLegacy);

  Json list = toolsListResult();
  const Json* tools = list.find("tools");
  CHECK(tools && tools->isArray());
  CHECK_EQ(tools->size(), (size_t)2);
  CHECK(list.find("ttlMs") != nullptr);
  CHECK_EQ(list.find("cacheScope")->asString(), std::string("public"));
  for (const auto& t : tools->items()) {
    const std::string n = t.find("name")->asString();
    CHECK(n == "search_files" || n == "index_stats");
    const Json* ann = t.find("annotations");
    CHECK(ann && ann->find("readOnlyHint")->asBool());
    CHECK(!ann->find("destructiveHint")->asBool());
    CHECK(ann->find("idempotentHint")->asBool());
    CHECK(!ann->find("openWorldHint")->asBool());
  }

  Json err = buildErrorResponse(Json::integer(7), kInvalidParams, "bad");
  CHECK_EQ(err.find("error")->find("code")->asInt(), (long long)kInvalidParams);

  Page p = paginate(pageSample(), 0, 2);
  Json payload = searchPayload(p, true);
  CHECK_EQ(payload.find("results")->size(), (size_t)2);
  const Json* page = payload.find("page");
  CHECK_EQ(page->find("offset")->asInt(), 0);
  CHECK_EQ(page->find("limit")->asInt(), 2);
  CHECK_EQ(page->find("returned")->asInt(), 2);
  CHECK(page->find("has_more")->asBool());
  CHECK(page->find("rebuilding")->asBool());

  std::vector<std::pair<std::string, std::string>> kv = {
      {"files", "7"}, {"dirs", "2"}, {"roots", "/home/x"}, {"rebuilding", "1"}};
  Json stats = statsPayload(kv);
  CHECK_EQ(stats.find("files")->asInt(), 7);
  CHECK_EQ(stats.find("roots")->asString(), std::string("/home/x"));
  CHECK(stats.find("rebuilding")->asBool());
  CHECK(!stats.find("version")->asString().empty());
}

TEST(mcp_json_surrogate_and_nul) {
  Json out;
  std::string err;
  // 代理对 -> U+1F600
  CHECK(Json::parse("\"\\uD83D\\uDE00\"", out, err));
  CHECK_EQ(out.asString(), std::string("\xF0\x9F\x98\x80"));
  // 孤立高/低代理 -> U+FFFD
  CHECK(Json::parse("\"\\uD800\"", out, err));
  CHECK_EQ(out.asString(), std::string("\xEF\xBF\xBD"));
  CHECK(Json::parse("\"\\uDC00\"", out, err));
  CHECK_EQ(out.asString(), std::string("\xEF\xBF\xBD"));
  // \u0000 往返
  CHECK(Json::parse("\"\\u0000\"", out, err));
  CHECK_EQ(out.asString().size(), (size_t)1);
  CHECK(out.asString()[0] == '\0');
  CHECK_EQ(out.dump(), std::string("\"\\u0000\""));
  CHECK(Json::parse("\"a\\u0000b\"", out, err));
  CHECK_EQ(out.asString().size(), (size_t)3);
  CHECK(out.asString()[1] == '\0');
}

TEST(mcp_json_depth_limit) {
  Json out;
  std::string err;
  std::string shallow;
  for (int i = 0; i < 10; ++i) shallow += "[";
  for (int i = 0; i < 10; ++i) shallow += "]";
  CHECK(Json::parse(shallow, out, err));

  std::string deep;
  for (int i = 0; i < 130; ++i) deep += "[";
  for (int i = 0; i < 130; ++i) deep += "]";
  CHECK(!Json::parse(deep, out, err));
}

TEST(mcp_json_nonfinite_rejected) {
  Json out;
  std::string err;
  CHECK(!Json::parse("1e999", out, err));
  CHECK(!Json::parse("-1e999", out, err));
  CHECK(!Json::parse("{\"a\":1e999}", out, err));
  // 有限的大数仍可解析
  CHECK(Json::parse("1e300", out, err));
  CHECK(out.isNumber());
}

TEST(mcp_json_asint_clamp) {
  Json out;
  std::string err;
  CHECK(Json::parse("1e300", out, err));
  CHECK_EQ(out.asInt(), std::numeric_limits<long long>::max());
  CHECK(Json::parse("-1e300", out, err));
  CHECK_EQ(out.asInt(), std::numeric_limits<long long>::min());
  Json ninf = Json::number(std::numeric_limits<double>::infinity());
  CHECK_EQ(ninf.asInt(42), (long long)42);
  CHECK_EQ(ninf.dump(), std::string("0"));
}

TEST(mcp_query_control_chars_rejected) {  SearchArgs sa;
  std::string err;
  Json inj = Json::object();
  inj.set("query", Json::str("x\nshutdown"));
  CHECK(!parseSearchArgs(inj, sa, err));
  Json tab = Json::object();
  tab.set("query", Json::str("a\tb"));
  CHECK(!parseSearchArgs(tab, sa, err));
  Json nul = Json::object();
  nul.set("query", Json::str(std::string("a\0b", 3)));
  CHECK(!parseSearchArgs(nul, sa, err));
  Json cr = Json::object();
  cr.set("query", Json::str("a\rb"));
  CHECK(!parseSearchArgs(cr, sa, err));
  // 含空格与 CJK 的合法查询仍通过
  Json good = Json::object();
  good.set("query", Json::str("年度 report"));
  CHECK(parseSearchArgs(good, sa, err));
}

TEST(mcp_parse_regex_query) {
  SearchArgs sa;
  std::string err;
  Json good = Json::object();
  good.set("query", Json::str("re:^a.*b$"));
  CHECK(parseSearchArgs(good, sa, err));
  CHECK_EQ(sa.query, std::string("re:^a.*b$"));

  Json bad = Json::object();
  bad.set("query", Json::str("re:["));
  CHECK(!parseSearchArgs(bad, sa, err));

  Json empty = Json::object();
  empty.set("query", Json::str("re:"));
  CHECK(!parseSearchArgs(empty, sa, err));

  Json blank = Json::object();
  blank.set("query", Json::str("re:   "));
  CHECK(!parseSearchArgs(blank, sa, err));
}

TEST(mcp_regex_paging_orthogonal) {
  // MCP 层对"整页"会回到 cap 取确定全局前缀（见 mcp/main.cpp）；这里模拟该行为再用
  // paginate 切片，验证与全量查询的排序前缀一致。匹配数 > limit 的子集不确定性不在断言内。
  std::vector<FileEntry> v;
  for (int i = 0; i < 6; ++i)
    v.push_back({"/a/rx_" + std::to_string(i), "rx_" + std::to_string(i), 1, i, false,
                 static_cast<uint64_t>(i)});
  Index idx;
  idx.build(std::move(v));
  std::vector<SearchResult> full, fetched;
  bool tr;
  idx.search("re:.", SortKey::Name, 0, false, false, full, tr);
  CHECK_EQ(full.size(), (size_t)6);
  idx.search("re:.", SortKey::Name, 50000, false, false, fetched, tr);  // cap 取全量：确定前缀
  CHECK_EQ(fetched.size(), (size_t)6);

  Page p0 = paginate(fetched, 0, 2);
  Page p1 = paginate(fetched, 2, 2);
  CHECK_EQ(p0.items.size(), (size_t)2);
  CHECK_EQ(p1.items.size(), (size_t)2);
  CHECK_EQ(p0.items[0].entry.name, full[0].entry.name);
  CHECK_EQ(p0.items[1].entry.name, full[1].entry.name);
  CHECK_EQ(p1.items[0].entry.name, full[2].entry.name);
  CHECK_EQ(p1.items[1].entry.name, full[3].entry.name);
}
