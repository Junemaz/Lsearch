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
  idx.search("b", SortKey::Name, 0, true, false, out, tr);
  if (!out.empty())
    for (auto& r : out) CHECK(r.entry.is_dir);
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
