#include "test_util.h"

#include "core/search.h"
#include "core/util.h"

#include <string>
#include <vector>

using namespace lsearch;

static std::vector<FileEntry> sample() {
  std::vector<FileEntry> v;
  v.push_back({"/a/report.pdf", "report.pdf", 1200, 100, false, 1});
  v.push_back({"/a/Report.docx", "Report.docx", 500, 200, false, 2});
  v.push_back({"/b/reports", "reports", 0, 300, true, 3});
  v.push_back({"/b/Readme.md", "Readme.md", 42, 400, false, 4});
  return v;
}

TEST(search_substring_case_insensitive) {
  Index idx;
  idx.build(sample());
  std::vector<SearchResult> out;
  bool tr;
  idx.search("REPORT", SortKey::Name, 0, false, false, out, tr);
  CHECK(out.size() >= 2);  // report.pdf + reports
}

TEST(search_wildcard) {
  Index idx;
  idx.build(sample());
  std::vector<SearchResult> out;
  bool tr;
  idx.search("*.p?f", SortKey::Path, 0, false, false, out, tr);
  CHECK_EQ(out.size(), (size_t)1);
  CHECK_EQ(out[0].entry.path, "/a/report.pdf");
}

TEST(search_sort_size) {
  Index idx;
  idx.build(sample());
  std::vector<SearchResult> out;
  bool tr;
  idx.search("re", SortKey::Size, 0, false, false, out, tr);
  CHECK(out.size() >= 3);
  CHECK_EQ(out[0].entry.path, "/a/report.pdf");  // 最大
}

TEST(search_dirs_only) {
  Index idx;
  idx.build(sample());
  std::vector<SearchResult> out;
  bool tr;
  // 名字含 report 的目录：/b/reports
  idx.search("report", SortKey::Name, 0, true, false, out, tr);
  CHECK_EQ(out.size(), (size_t)1);
  CHECK(out[0].entry.is_dir);
  CHECK_EQ(out[0].entry.path, "/b/reports");
}

TEST(search_name_only_not_path) {
  // 回归：路径含 code、但文件名不含 code 的条目，搜索 code 不应命中
  std::vector<FileEntry> v;
  v.push_back({"/home/code/DeepSeek/main.cpp", "main.cpp", 1, 1, false, 1});
  v.push_back({"/home/code/Project/codebook.txt", "codebook.txt", 1, 1, false, 2});
  Index idx;
  idx.build(std::move(v));
  std::vector<SearchResult> out;
  bool tr;
  idx.search("code", SortKey::Name, 0, false, false, out, tr);
  CHECK_EQ(out.size(), (size_t)1);
  CHECK_EQ(out[0].entry.path, "/home/code/Project/codebook.txt");
  CHECK(out[0].entry.name == "codebook.txt");
}

TEST(search_empty_query) {
  Index idx;
  idx.build(sample());
  std::vector<SearchResult> out;
  bool tr;
  idx.search("", SortKey::Name, 0, false, false, out, tr);
  CHECK(out.empty());
}

TEST(search_limit) {
  Index idx;
  idx.build(sample());
  std::vector<SearchResult> out;
  bool tr;
  idx.search("e", SortKey::Path, 2, false, false, out, tr);  // 至少 3 个含 e
  CHECK(out.size() <= 2);
}

TEST(index_add_remove) {
  Index idx;
  idx.build(sample());
  CHECK_EQ(idx.size(), (size_t)4);

  FileEntry e{"/a/new.txt", "new.txt", 10, 5, false, 9};
  idx.add(e);
  CHECK_EQ(idx.size(), (size_t)5);

  CHECK(idx.remove("/a/new.txt"));
  CHECK_EQ(idx.size(), (size_t)4);
  CHECK(!idx.remove("/a/new.txt"));
}

TEST(index_remove_subtree) {
  Index idx;
  idx.build(sample());
  size_t n = idx.removePathAndSubtree("/a");
  CHECK_EQ(n, (size_t)2);
  std::vector<SearchResult> out;
  bool tr;
  idx.search("report", SortKey::Path, 0, false, false, out, tr);
  CHECK_EQ(out.size(), (size_t)1);
  CHECK_EQ(out[0].entry.path, "/b/reports");
}

TEST(glob_match) {
  CHECK(globMatch("*.p?f", "report.pdf"));
  CHECK(globMatch("REPORT.*", "report.pdf"));
  CHECK(!globMatch("*.x", "report.pdf"));
  CHECK(globMatch("*", "anything"));
}

static std::vector<FileEntry> annualReports() {
  std::vector<FileEntry> v;
  v.push_back({"/a/AnnualReport.txt", "AnnualReport.txt", 1, 1, false, 1});
  v.push_back({"/a/AnnualReport2026.txt", "AnnualReport2026.txt", 1, 2, false, 2});
  v.push_back({"/a/vacation_photo.jpg", "vacation_photo.jpg", 1, 4, false, 4});
  return v;
}

TEST(search_regex_anchored) {
  Index idx;
  idx.build(annualReports());
  std::vector<SearchResult> out;
  bool tr;
  idx.search("re:^AnnualReport\\d{4}\\.txt$", SortKey::Name, 0, false, false, out, tr);
  CHECK_EQ(out.size(), (size_t)1);
  CHECK_EQ(out[0].entry.name, "AnnualReport2026.txt");
}

TEST(search_regex_case_insensitive) {
  Index idx;
  idx.build(annualReports());
  std::vector<SearchResult> out;
  bool tr;
  idx.search("re:ANNUALREPORT", SortKey::Name, 0, false, false, out, tr);
  CHECK_EQ(out.size(), (size_t)2);  // AnnualReport.txt + AnnualReport2026.txt
}

TEST(search_validate_query) {
  std::string err;
  CHECK(!validateQuery("re:[", err));
  CHECK(!err.empty());
  err.clear();
  CHECK(validateQuery("re:^a.*b$", err) && err.empty());
  CHECK(validateQuery("re:", err));      // 空模式合法（空查询）
  CHECK(validateQuery("re:   ", err));   // 全空白合法
  CHECK(validateQuery("*.pdf", err));    // 非 re: 恒合法
  CHECK(validateQuery("report", err));
}

TEST(search_regex_empty_pattern) {
  Index idx;
  idx.build(annualReports());
  std::vector<SearchResult> out;
  bool tr;
  idx.search("re:", SortKey::Name, 0, false, false, out, tr);
  CHECK(out.empty());
  bool tr2;
  idx.search("re:   ", SortKey::Name, 0, false, false, out, tr2);
  CHECK(out.empty());
}

TEST(search_star_still_glob_not_regex) {
  // 非 re: 且含 * 的查询走 glob：若误走正则，"re 不能为空" 的 '*.p?f' 会编译失败返回空。
  Index idx;
  idx.build(sample());
  std::vector<SearchResult> out;
  bool tr;
  idx.search("*.p?f", SortKey::Name, 0, false, false, out, tr);
  CHECK_EQ(out.size(), (size_t)1);
  CHECK_EQ(out[0].entry.path, "/a/report.pdf");
}

TEST(search_regex_pattern_not_lowercased) {
  // 原始 pattern 不变量：若把 pattern 小写化，\W 会变成 \w，从而错误命中纯词字符名。
  std::vector<FileEntry> v;
  v.push_back({"/a/ABC", "ABC", 1, 1, false, 1});
  Index idx;
  idx.build(std::move(v));
  std::vector<SearchResult> out;
  bool tr;
  idx.search("re:\\W", SortKey::Name, 0, false, false, out, tr);
  CHECK(out.empty());  // "ABC" 无任何非词字符
  idx.search("re:\\w", SortKey::Name, 0, false, false, out, tr);
  CHECK_EQ(out.size(), (size_t)1);
}

TEST(search_regex_non_ascii_and_icase) {
  std::vector<FileEntry> v;
  v.push_back({"/a/报告.txt", "报告.txt", 1, 1, false, 1});
  v.push_back({"/a/REPORT", "REPORT", 1, 2, false, 2});
  Index idx;
  idx.build(std::move(v));
  std::vector<SearchResult> out;
  bool tr;
  idx.search("re:^报告\\.txt$", SortKey::Name, 0, false, false, out, tr);
  CHECK_EQ(out.size(), (size_t)1);
  CHECK_EQ(out[0].entry.name, "报告.txt");
  idx.search("re:report", SortKey::Name, 0, false, false, out, tr);
  CHECK_EQ(out.size(), (size_t)1);
  CHECK_EQ(out[0].entry.name, "REPORT");  // ASCII icase 仍生效
}

TEST(search_regex_paging_prefix) {
  // 新语义（searchEx）：匹配数 ≤ cap 时收集全部匹配 → total 精确、页间稳定；
  // limit<匹配数 时返回排序后的确定前缀，而非不确定子集。
  std::vector<FileEntry> v;
  for (int i = 0; i < 6; ++i)
    v.push_back({"/a/rx_" + std::to_string(i), "rx_" + std::to_string(i), 1, i, false,
                 static_cast<uint64_t>(i)});
  Index idx;
  idx.build(std::move(v));

  SearchOutcome full;
  idx.searchEx("re:.", SortKey::Name, 0, false, false, "", full);
  CHECK_EQ(full.results.size(), (size_t)6);
  CHECK_EQ(full.total, (uint64_t)6);
  CHECK(!full.total_capped);

  SearchOutcome page;
  idx.searchEx("re:.", SortKey::Name, 6, false, false, "", page);
  CHECK_EQ(page.results.size(), (size_t)6);
  for (size_t i = 0; i < page.results.size(); ++i)
    CHECK_EQ(page.results[i].entry.name, full.results[i].entry.name);

  SearchOutcome limited;
  idx.searchEx("re:.", SortKey::Name, 3, false, false, "", limited);
  CHECK_EQ(limited.results.size(), (size_t)3);
  CHECK_EQ(limited.total, (uint64_t)6);  // total 仍精确
  CHECK(!limited.total_capped);
  for (size_t i = 0; i < limited.results.size(); ++i)
    CHECK_EQ(limited.results[i].entry.name, full.results[i].entry.name);  // 确定前缀
}

static std::vector<FileEntry> underSample() {
  std::vector<FileEntry> v;
  v.push_back({"/data", "data", 0, 1, true, 1});
  v.push_back({"/data/report.txt", "report.txt", 1, 2, false, 2});
  v.push_back({"/data/sub/report.txt", "report.txt", 1, 3, false, 3});
  v.push_back({"/data2/report.txt", "report.txt", 1, 4, false, 4});
  v.push_back({"/datax/report.txt", "report.txt", 1, 5, false, 5});
  v.push_back({"/Data/report.txt", "report.txt", 1, 6, false, 6});
  v.push_back({"/other/report.txt", "report.txt", 1, 7, false, 7});
  return v;
}

TEST(search_ex_under_exact_and_subtree) {
  Index idx;
  idx.build(underSample());
  SearchOutcome out;
  idx.searchEx("report", SortKey::Path, 0, false, false, "/data", out);
  CHECK_EQ(out.total, (uint64_t)2);  // /data/report.txt + /data/sub/report.txt
  CHECK(!out.total_capped);
  CHECK_EQ(out.results.size(), (size_t)2);
  for (const auto& r : out.results) CHECK(startsWith(r.entry.path, "/data/"));

  // path == under：目录条目本身命中
  idx.searchEx("data", SortKey::Path, 0, false, false, "/data", out);
  CHECK_EQ(out.total, (uint64_t)1);
  CHECK_EQ(out.results[0].entry.path, std::string("/data"));
}

TEST(search_ex_under_sibling_guard) {
  Index idx;
  idx.build(underSample());
  SearchOutcome out;
  idx.searchEx("report", SortKey::Path, 0, false, false, "/data", out);
  for (const auto& r : out.results) {
    CHECK(r.entry.path.rfind("/data2/", 0) != 0);
    CHECK(r.entry.path.rfind("/datax/", 0) != 0);
  }
}

TEST(search_ex_under_no_match) {
  Index idx;
  idx.build(underSample());
  SearchOutcome out;
  idx.searchEx("report", SortKey::Path, 0, false, false, "/nope", out);
  CHECK_EQ(out.total, (uint64_t)0);
  CHECK(out.results.empty());
  CHECK(!out.total_capped);
}

TEST(search_ex_under_all_variants) {
  Index idx;
  idx.build(underSample());
  SearchOutcome a, b, c, d, e;
  idx.searchEx("report", SortKey::Path, 0, false, false, "", a);
  idx.searchEx("report", SortKey::Path, 0, false, false, "/", b);
  idx.searchEx("report", SortKey::Path, 0, false, false, "-", c);
  // 规范化：连续/多余斜杠折叠为 "/"（=全量）；已在 Spec 010 R1 明确记录。
  idx.searchEx("report", SortKey::Path, 0, false, false, "//", d);
  idx.searchEx("report", SortKey::Path, 0, false, false, "///", e);
  CHECK_EQ(a.total, (uint64_t)6);
  CHECK_EQ(b.total, (uint64_t)6);
  CHECK_EQ(c.total, (uint64_t)6);
  CHECK_EQ(d.total, (uint64_t)6);
  CHECK_EQ(e.total, (uint64_t)6);
}

TEST(search_ex_under_trailing_slash) {
  Index idx;
  idx.build(underSample());
  SearchOutcome a, b;
  idx.searchEx("report", SortKey::Path, 0, false, false, "/data", a);
  idx.searchEx("report", SortKey::Path, 0, false, false, "/data/", b);
  CHECK_EQ(a.total, b.total);
  CHECK_EQ(a.results.size(), b.results.size());
}

TEST(search_ex_under_case_sensitive) {
  Index idx;
  idx.build(underSample());
  SearchOutcome lower, upper;
  idx.searchEx("report", SortKey::Path, 0, false, false, "/data", lower);
  for (const auto& r : lower.results) CHECK(r.entry.path.rfind("/Data/", 0) != 0);
  idx.searchEx("report", SortKey::Path, 0, false, false, "/Data", upper);
  CHECK_EQ(upper.total, (uint64_t)1);
  CHECK_EQ(upper.results[0].entry.path, std::string("/Data/report.txt"));
}

TEST(search_ex_under_orthogonal) {
  Index idx;
  idx.build(underSample());
  SearchOutcome re, glob, dirs, files;
  idx.searchEx("re:^report", SortKey::Path, 0, false, false, "/data", re);
  CHECK_EQ(re.total, (uint64_t)2);
  idx.searchEx("*.txt", SortKey::Path, 0, false, false, "/data", glob);
  CHECK_EQ(glob.total, (uint64_t)2);
  idx.searchEx("data", SortKey::Path, 0, true, false, "/data", dirs);
  CHECK_EQ(dirs.total, (uint64_t)1);
  CHECK(dirs.results[0].entry.is_dir);
  idx.searchEx("report", SortKey::Path, 0, false, true, "/data", files);
  CHECK_EQ(files.total, (uint64_t)2);
}

TEST(search_ex_offset_returned_total) {
  Index idx;
  idx.build(underSample());
  SearchOutcome out;
  idx.searchEx("report", SortKey::Path, 3, false, false, "", out);
  CHECK_EQ(out.results.size(), (size_t)3);
  CHECK_EQ(out.total, (uint64_t)6);
  CHECK(!out.total_capped);
}

TEST(search_ex_cap_flags_membership) {
  std::vector<FileEntry> v;
  for (int i = 0; i < 8; ++i)
    v.push_back({"/c/e" + std::to_string(i), "e" + std::to_string(i), 1, i, false,
                 static_cast<uint64_t>(i)});
  Index idx;
  idx.build(std::move(v));
  const size_t old = idx.setCandidateCapForTest(3);
  SearchOutcome out;
  idx.searchEx("e", SortKey::Name, 0, false, false, "", out);
  CHECK(out.total_capped);
  CHECK_EQ(out.total, (uint64_t)3);
  CHECK(out.results.size() <= 3);
  for (const auto& r : out.results) CHECK(r.entry.path.rfind("/c/", 0) == 0);
  idx.setCandidateCapForTest(old);
}

TEST(search_ex_exact_cap_conservative) {
  // 恰好 == cap 个匹配也无法与"还有更多"区分：扫描在 cap 处停止 → 保守标记 capped。
  // 仅断言 flags + membership（Flaky 规则）。
  std::vector<FileEntry> v;
  for (int i = 0; i < 4; ++i)
    v.push_back({"/c/match" + std::to_string(i), "match" + std::to_string(i), 1, i, false,
                 static_cast<uint64_t>(i)});
  for (int i = 0; i < 96; ++i)
    v.push_back({"/c/other" + std::to_string(i), "other" + std::to_string(i), 1, i, false,
                 static_cast<uint64_t>(i)});
  Index idx;
  idx.build(std::move(v));
  const size_t old = idx.setCandidateCapForTest(4);
  SearchOutcome out;
  idx.searchEx("match", SortKey::Name, 0, false, false, "", out);
  CHECK(out.total_capped);
  CHECK_EQ(out.total, (uint64_t)4);
  for (const auto& r : out.results) CHECK(startsWith(r.entry.name, "match"));
  idx.setCandidateCapForTest(old);
}

TEST(count_ex_exact) {
  Index idx;
  idx.build(underSample());
  bool capped = true;
  CHECK_EQ(idx.countEx("report", false, false, "", capped), (size_t)6);
  CHECK(!capped);
  CHECK_EQ(idx.countEx("report", false, false, "/data", capped), (size_t)2);
  CHECK(!capped);
  CHECK_EQ(idx.countEx("report", true, false, "/data", capped), (size_t)0);
  CHECK(!capped);
}

TEST(count_ex_cap) {
  std::vector<FileEntry> v;
  for (int i = 0; i < 8; ++i)
    v.push_back({"/c/e" + std::to_string(i), "e" + std::to_string(i), 1, i, false,
                 static_cast<uint64_t>(i)});
  Index idx;
  idx.build(std::move(v));
  const size_t old = idx.setCandidateCapForTest(3);
  bool capped = false;
  const size_t n = idx.countEx("e", false, false, "", capped);
  CHECK(capped);
  CHECK_EQ(n, (size_t)3);
  idx.setCandidateCapForTest(old);
}

TEST(index_total_bytes_no_overflow) {
  std::vector<FileEntry> v;
  v.push_back({"/a/big1", "big1", INT64_MAX, 1, false, 1});
  v.push_back({"/a/big2", "big2", INT64_MAX - 5, 1, false, 2});
  v.push_back({"/a/neg", "neg", -42, 1, false, 3});
  v.push_back({"/a/normal", "normal", 100, 1, false, 4});
  Index idx;
  idx.build(std::move(v));
  // 荒谬/负 size 归零，仅正常值计入；且总量远小于 2^63
  CHECK_EQ(idx.totalBytes(), (uint64_t)100);
}

TEST(index_total_bytes_saturates) {
  // 1<<50 * 2^14 == 2^64：无饱和时回绕为 0；饱和后应为 UINT64_MAX。
  std::vector<FileEntry> v;
  v.reserve(1u << 14);
  for (int i = 0; i < (1 << 14); ++i)
    v.push_back({"/a/f" + std::to_string(i), "f" + std::to_string(i),
                 kMaxPlausibleFileSize, 1, false, static_cast<uint64_t>(i)});
  Index idx;
  idx.build(std::move(v));
  CHECK_EQ(idx.totalBytes(), (uint64_t)UINT64_MAX);
  CHECK(idx.totalBytes() != 0);
}

TEST(index_add_update_bytes_correct) {
  FileEntry a{"/a/x", "x", 1000, 1, false, 1};
  Index idx;
  idx.build(std::vector<FileEntry>{a});
  CHECK_EQ(idx.totalBytes(), (uint64_t)1000);
  // 原地更新必须替换旧值（曾漏加新值 -> 单调下溢 -> stats.size 报 2^63）
  idx.add({"/a/x", "x", 250, 2, false, 1});
  CHECK_EQ(idx.size(), (size_t)1);
  CHECK_EQ(idx.totalBytes(), (uint64_t)250);
  for (int i = 0; i < 1000; ++i) idx.add({"/a/x", "x", 300, 3, false, 1});
  CHECK_EQ(idx.totalBytes(), (uint64_t)300);
  // 删除后归零，不得下溢
  idx.remove("/a/x");
  CHECK_EQ(idx.totalBytes(), (uint64_t)0);
}

TEST(base64_roundtrip_and_strict) {
  CHECK_EQ(base64Encode(""), std::string(""));
  CHECK_EQ(base64Encode("f"), std::string("Zg=="));
  CHECK_EQ(base64Encode("fo"), std::string("Zm8="));
  CHECK_EQ(base64Encode("foo"), std::string("Zm9v"));
  CHECK_EQ(base64Encode("hello"), std::string("aGVsbG8="));

  std::string d;
  CHECK(base64Decode("aGVsbG8=", d));
  CHECK_EQ(d, std::string("hello"));
  CHECK(base64Decode("", d));
  CHECK(d.empty());

  CHECK(!base64Decode("aGVsbG8", d));         // 长度非 4 倍数
  CHECK(!base64Decode("aGVs bG8", d));        // 空白
  CHECK(!base64Decode("aGVsbG8*", d));        // 非法字符
  CHECK(!base64Decode("====", d));            // 非法填充
  CHECK(!base64Decode("AB==", d));            // 非规范填充位
  CHECK(!base64Decode("aGVsbG8=" + std::string(kBase64MaxEncoded, 'A'), d));  // 超长

  const std::string nul = base64Encode(std::string("a\0b", 3));
  CHECK(!base64Decode(nul, d));  // 解码含 NUL

  const std::string cjk = "/数据/报告";
  CHECK(base64Decode(base64Encode(cjk), d));
  CHECK_EQ(d, cjk);
}
