#pragma once
// lsearchd 守护进程：持有 SQLite 持久化 + 内存热索引 + inotify 增量更新，
// 通过 Unix socket 对外提供搜索/统计/重建等能力（GUI/TUI/CLI 共享）。
#include "core/config.h"
#include "core/db.h"
#include "core/search.h"
#include "core/watcher.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <shared_mutex>
#include <string>

namespace lsearch {

inline constexpr const char* kVersion = "0.1.0";

class Daemon {
 public:
  explicit Daemon(Config cfg);
  ~Daemon();

  // 打开数据库并加载/构建索引
  bool init(std::string& err);

  // 启动 inotify 与 IPC 服务（阻塞直到 shutdown）
  bool run(std::string& err);

  void shutdown();

  bool running() const { return running_.load(); }

 private:
  void handleRequest(const std::string& line, std::string& out);
  void buildSearchResponse(const std::string& line, std::string& out);
  void serveConnection(int fd);

  void applyWatch(const WatchEvent& ev);
  bool doFullScan();
  bool loadIndexFromDb();
  void rebuildAsync();
  void restartWatcher();
  void startRebuild();

  Config cfg_;
  Db db_;
  Index idx_;
  std::shared_mutex idxLock_;
  Watcher watcher_;
  int listenFd_ = -1;
  std::string sockPath_;

  std::atomic<bool> running_{true};
  std::atomic<int64_t> startedAt_{0};
  std::mutex rebuildM_;
  bool rebuilding_ = false;
};

}  // namespace lsearch
