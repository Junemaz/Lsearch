#pragma once
// SQLite 持久化：文件索引表 + 元数据表
#include "core/entry.h"

#include <sqlite3.h>

#include <string>
#include <vector>

namespace lsearch {

class Db {
 public:
  Db() = default;
  ~Db();
  Db(const Db&) = delete;
  Db& operator=(const Db&) = delete;

  bool open(const std::string& path, std::string& err);
  bool isOpen() const { return db_ != nullptr; }
  void close();

  bool begin();
  bool commit();
  bool rollback();

  // 单条增删改（配合事务批量使用）
  bool upsert(const FileEntry& e);
  bool remove(const std::string& path);

  // 删除 path 及其所有后代（目录移除）
  bool removeSubtree(const std::string& path);

  // 清空 files 表（全量重建前调用）
  bool clearFiles();

  // 全量载入到内存（守护进程启动时快速恢复索引）
  bool loadAll(std::vector<FileEntry>& out);

  int64_t countFiles();
  // Spec 017 sqlite 模式：stats 与 watcher 目录列表均来自 DB（不常驻全量条目）。
  int64_t countDirs();
  // 与 Index::totalBytes 同语义：逐条 sanitizeSize 后求和（SQL 侧做同规则归零）。
  int64_t sumSanitizedBytes();
  bool loadDirs(std::vector<std::string>& out);
  std::string getMeta(const std::string& key, const std::string& def = "");
  bool setMeta(const std::string& key, const std::string& val);

  sqlite3* raw() { return db_; }

 private:
  bool exec(const char* sql);
  sqlite3* db_ = nullptr;
};

}  // namespace lsearch
