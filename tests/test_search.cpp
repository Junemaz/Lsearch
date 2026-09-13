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
  // 已知限制：匹配数 > limit 时引擎返回不确定子集，因此只在 limit >= 匹配数
  //（确定返回全集）下断言排序一致，另单独验证 limit<匹配数的子集性质。
  std::vector<FileEntry> v;
  for (int i = 0; i < 6; ++i)
    v.push_back({"/a/rx_" + std::to_string(i), "rx_" + std::to_string(i), 1, i, false,
                 static_cast<uint64_t>(i)});
  Index idx;
  idx.build(std::move(v));
  std::vector<SearchResult> full, page, capped;
  bool tr;
  idx.search("re:.", SortKey::Name, 0, false, false, full, tr);
  CHECK_EQ(full.size(), (size_t)6);
  idx.search("re:.", SortKey::Name, 6, false, false, page, tr);
  CHECK_EQ(page.size(), (size_t)6);
  for (size_t i = 0; i < page.size(); ++i) CHECK_EQ(page[i].entry.name, full[i].entry.name);
  idx.search("re:.", SortKey::Name, 3, false, false, capped, tr);
  CHECK_EQ(capped.size(), (size_t)3);
  for (const auto& r : capped) {
    bool in_full = false;
    for (const auto& f : full)
      if (f.entry.name == r.entry.name) in_full = true;
    CHECK(in_full);
  }
}
