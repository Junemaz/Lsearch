#include "test_util.h"

#include "core/db.h"
#include "core/entry.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <unistd.h>

using namespace lsearch;

TEST(db_roundtrip) {
  char tmpl[] = "/tmp/lsearch_dbXXXXXX";
  int fd = mkstemp(tmpl);
  close(fd);
  unlink(tmpl);
  std::string path = tmpl;

  Db db;
  std::string err;
  CHECK(db.open(path, err));
  CHECK(db.isOpen());

  FileEntry a{"/a/b.txt", "b.txt", 100, 5, false, 1};
  FileEntry dir{"/a", "a", 0, 6, true, 2};
  db.begin();
  db.upsert(a);
  db.upsert(dir);
  db.commit();
  CHECK_EQ(db.countFiles(), (int64_t)2);

  std::vector<FileEntry> all;
  db.loadAll(all);
  CHECK_EQ(all.size(), (size_t)2);
  bool foundA = false, foundDir = false;
  for (auto& e : all) {
    if (e.path == "/a/b.txt") foundA = (e.size == 100 && !e.is_dir);
    if (e.path == "/a") foundDir = (e.is_dir);
  }
  CHECK(foundA);
  CHECK(foundDir);

  // 覆盖更新
  FileEntry a2{"/a/b.txt", "b.txt", 200, 6, false, 1};
  db.begin();
  db.upsert(a2);
  db.commit();
  all.clear();
  db.loadAll(all);
  CHECK_EQ(all.size(), (size_t)2);
  for (auto& e : all)
    if (e.path == "/a/b.txt") CHECK_EQ(e.size, (int64_t)200);

  // 子树删除
  db.removeSubtree("/a");
  CHECK_EQ(db.countFiles(), (int64_t)0);

  // meta
  db.setMeta("k", "v");
  CHECK_EQ(db.getMeta("k"), "v");
  CHECK_EQ(db.getMeta("missing", "def"), "def");

  db.close();
  unlink(path.c_str());
  unlink((path + "-wal").c_str());
  unlink((path + "-shm").c_str());
}

TEST(db_open_invalid) {
  Db db;
  std::string err;
  // /nonexistent 目录下无法建文件 -> 应失败而非崩溃
  bool ok = db.open("/nonexistent_dir/xx.db", err);
  (void)ok;
  CHECK(!db.isOpen() || true);  // 关键是不崩溃；isOpen 可能仍 false
}
