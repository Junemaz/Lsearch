#include "core/db.h"

#include <cstdio>

namespace lsearch {

Db::~Db() { close(); }

void Db::close() {
  if (db_) {
    sqlite3_close(db_);
    db_ = nullptr;
  }
}

bool Db::exec(const char* sql) {
  char* err = nullptr;
  int rc = sqlite3_exec(db_, sql, nullptr, nullptr, &err);
  if (rc != SQLITE_OK) {
    if (err) sqlite3_free(err);
    return false;
  }
  return true;
}

bool Db::open(const std::string& path, std::string& err) {
  int rc = sqlite3_open_v2(path.c_str(), &db_,
                           SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr);
  if (rc != SQLITE_OK) {
    err = std::string("sqlite open failed: ") + sqlite3_errmsg(db_);
    close();
    return false;
  }
  // WAL 模式：读写并发友好
  exec("PRAGMA journal_mode=WAL;");
  exec("PRAGMA synchronous=NORMAL;");
  exec("PRAGMA busy_timeout=5000;");

  const char* schema =
      "CREATE TABLE IF NOT EXISTS files("
      " path TEXT PRIMARY KEY,"
      " name TEXT NOT NULL,"
      " size INTEGER NOT NULL DEFAULT 0,"
      " mtime INTEGER NOT NULL DEFAULT 0,"
      " is_dir INTEGER NOT NULL DEFAULT 0,"
      " inode INTEGER NOT NULL DEFAULT 0);"
      "CREATE INDEX IF NOT EXISTS idx_files_name ON files(name);"
      "CREATE TABLE IF NOT EXISTS meta(k TEXT PRIMARY KEY, v TEXT);";
  if (!exec(schema)) {
    err = "failed to create schema";
    return false;
  }
  return true;
}

bool Db::begin() { return exec("BEGIN IMMEDIATE;"); }
bool Db::commit() { return exec("COMMIT;"); }
bool Db::rollback() { return exec("ROLLBACK;"); }

bool Db::upsert(const FileEntry& e) {
  const char* sql =
      "INSERT INTO files(path,name,size,mtime,is_dir,inode) VALUES(?1,?2,?3,?4,?5,?6)"
      " ON CONFLICT(path) DO UPDATE SET name=excluded.name,size=excluded.size,"
      " mtime=excluded.mtime,is_dir=excluded.is_dir,inode=excluded.inode;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
  sqlite3_bind_text(st, 1, e.path.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 2, e.name.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 3, e.size);
  sqlite3_bind_int64(st, 4, e.mtime);
  sqlite3_bind_int(st, 5, e.is_dir ? 1 : 0);
  sqlite3_bind_int64(st, 6, static_cast<sqlite3_int64>(e.inode));
  int rc = sqlite3_step(st);
  sqlite3_finalize(st);
  return rc == SQLITE_DONE;
}

bool Db::remove(const std::string& path) {
  const char* sql = "DELETE FROM files WHERE path=?1;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
  sqlite3_bind_text(st, 1, path.c_str(), -1, SQLITE_TRANSIENT);
  int rc = sqlite3_step(st);
  sqlite3_finalize(st);
  return rc == SQLITE_DONE;
}

static std::string likeEscape(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (char c : s) {
    if (c == '\\' || c == '%' || c == '_') out.push_back('\\');
    out.push_back(c);
  }
  return out;
}

bool Db::removeSubtree(const std::string& path) {
  const char* sql =
      "DELETE FROM files WHERE path=?1 OR path LIKE ?2 ESCAPE '\\';";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
  std::string escaped = likeEscape(path);
  std::string pattern = escaped + "/%";
  sqlite3_bind_text(st, 1, path.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 2, pattern.c_str(), -1, SQLITE_TRANSIENT);
  int rc = sqlite3_step(st);
  sqlite3_finalize(st);
  return rc == SQLITE_DONE;
}

bool Db::clearFiles() { return exec("DELETE FROM files;"); }

bool Db::loadAll(std::vector<FileEntry>& out) {
  out.clear();
  const char* sql =
      "SELECT path,name,size,mtime,is_dir,inode FROM files;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
  while (sqlite3_step(st) == SQLITE_ROW) {
    FileEntry e;
    const unsigned char* path = sqlite3_column_text(st, 0);
    const unsigned char* name = sqlite3_column_text(st, 1);
    e.path.assign(reinterpret_cast<const char*>(path),
                  static_cast<size_t>(sqlite3_column_bytes(st, 0)));
    e.name.assign(reinterpret_cast<const char*>(name),
                  static_cast<size_t>(sqlite3_column_bytes(st, 1)));
    e.size = sqlite3_column_int64(st, 2);
    e.mtime = sqlite3_column_int64(st, 3);
    e.is_dir = sqlite3_column_int(st, 4) != 0;
    e.inode = static_cast<uint64_t>(sqlite3_column_int64(st, 5));
    out.push_back(std::move(e));
  }
  sqlite3_finalize(st);
  return true;
}

int64_t Db::countFiles() {
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, "SELECT COUNT(*) FROM files;", -1, &st, nullptr) != SQLITE_OK)
    return -1;
  int64_t n = (sqlite3_step(st) == SQLITE_ROW) ? sqlite3_column_int64(st, 0) : -1;
  sqlite3_finalize(st);
  return n;
}

int64_t Db::countDirs() {
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, "SELECT COUNT(*) FROM files WHERE is_dir=1;", -1, &st,
                         nullptr) != SQLITE_OK)
    return -1;
  int64_t n = (sqlite3_step(st) == SQLITE_ROW) ? sqlite3_column_int64(st, 0) : -1;
  sqlite3_finalize(st);
  return n;
}

int64_t Db::sumSanitizedBytes() {
  // 与 Index::build/add 的 sanitizeSize 规则一致：负值或 >1 PiB 归零后求和。
  const char* sql =
      "SELECT COALESCE(SUM(CASE WHEN size<0 OR size>?1 THEN 0 ELSE size END),0) FROM files;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return -1;
  sqlite3_bind_int64(st, 1, kMaxPlausibleFileSize);
  int64_t n = (sqlite3_step(st) == SQLITE_ROW) ? sqlite3_column_int64(st, 0) : -1;
  sqlite3_finalize(st);
  return n;
}

bool Db::loadDirs(std::vector<std::string>& out) {
  out.clear();
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, "SELECT path FROM files WHERE is_dir=1;", -1, &st, nullptr) !=
      SQLITE_OK)
    return false;
  while (sqlite3_step(st) == SQLITE_ROW) {
    const unsigned char* p = sqlite3_column_text(st, 0);
    if (p) out.emplace_back(reinterpret_cast<const char*>(p),
                            static_cast<size_t>(sqlite3_column_bytes(st, 0)));
  }
  sqlite3_finalize(st);
  return true;
}

std::string Db::getMeta(const std::string& key, const std::string& def) {
  sqlite3_stmt* st = nullptr;
  const char* sql = "SELECT v FROM meta WHERE k=?1;";
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return def;
  sqlite3_bind_text(st, 1, key.c_str(), -1, SQLITE_TRANSIENT);
  std::string out = def;
  if (sqlite3_step(st) == SQLITE_ROW) {
    const unsigned char* v = sqlite3_column_text(st, 0);
    if (v) out = reinterpret_cast<const char*>(v);
  }
  sqlite3_finalize(st);
  return out;
}

bool Db::setMeta(const std::string& key, const std::string& val) {
  const char* sql =
      "INSERT INTO meta(k,v) VALUES(?1,?2) ON CONFLICT(k) DO UPDATE SET v=excluded.v;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
  sqlite3_bind_text(st, 1, key.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 2, val.c_str(), -1, SQLITE_TRANSIENT);
  int rc = sqlite3_step(st);
  sqlite3_finalize(st);
  return rc == SQLITE_DONE;
}

}  // namespace lsearch
