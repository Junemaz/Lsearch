#include "test_util.h"

#include "core/config.h"
#include "core/util.h"

#include <cstdio>
#include <cstdlib>
#include <unistd.h>

using namespace lsearch;

TEST(config_defaults) {
  auto c = Config::defaults();
  CHECK(!c.paths.empty());
  CHECK(!c.paths[0].empty());
  CHECK(!c.index_hidden);
  CHECK(!c.follow_symlinks);
}

TEST(config_load_override) {
  char tmpl[] = "/tmp/lsearch_cfgXXXXXX";
  int fd = mkstemp(tmpl);
  close(fd);
  FILE* f = fopen(tmpl, "w");
  fputs("paths = /a, /b/c\n", f);
  fputs("excludes = /x,/y\n", f);
  fputs("index_hidden = 1\n", f);
  fputs("follow_symlinks = yes\n", f);
  fclose(f);

  auto c = Config::load(tmpl);
  CHECK_EQ(c.paths.size(), (size_t)2);
  CHECK_EQ(c.paths[0], "/a");
  CHECK_EQ(c.paths[1], "/b/c");
  CHECK_EQ(c.excludes.size(), (size_t)2);
  CHECK(c.index_hidden);
  CHECK(c.follow_symlinks);

  // exclude 前缀匹配
  CHECK(c.isExcluded("/x"));
  CHECK(c.isExcluded("/x/foo/bar"));
  CHECK(!c.isExcluded("/xyz"));

  c.save(tmpl);
  auto d = Config::load(tmpl);
  CHECK_EQ(d.paths.size(), (size_t)2);
  CHECK_EQ(d.paths[0], "/a");

  unlink(tmpl);
}

TEST(config_hidden) {
  auto c = Config::defaults();
  CHECK(c.isHiddenName(".git"));
  CHECK(!c.isHiddenName("git"));
  CHECK(!c.isHiddenName(""));
}
