#pragma once
// lsearchd 守护进程：持有 SQLite 持久化 + 内存热索引 + inotify 增量更新，
// 通过 Unix socket 对外提供搜索/统计/重建等能力（GUI/TUI/CLI 共享）。
#include "core/config.h"
#include "core/db.h"
#include "core/search.h"
#include "core/watcher.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <thread>
#include <vector>

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
  void buildSearchExResponse(const std::string& line, std::string& out);
  void buildSearch3Response(const std::string& line, std::string& out);
  void buildCountExResponse(const std::string& line, std::string& out);
  void serveConnection(int fd);

  void applyWatch(const WatchEvent& ev);
  bool scanAll(const Config& cfg, std::vector<FileEntry>& entries,
               const std::atomic<bool>* cancel, bool& aborted);
  bool commitScan(const Config& cfg, std::vector<FileEntry>&& entries,
                  const std::atomic<bool>* cancel, bool& aborted);
  bool doFullScan();
  bool loadIndexFromDb();
  void rebuildAsync();
  void restartWatcher();
  void restartWatcherLocked();
  void startRebuild();
  void finishRebuild();
  bool cancelAndWaitRebuild();

  Config cfg_;
  Db db_;
  Index idx_;
  std::shared_mutex idxLock_;
  Watcher watcher_;
  int listenFd_ = -1;
  std::string sockPath_;

  std::atomic<bool> running_{true};
  std::atomic<int64_t> startedAt_{0};
  std::atomic<bool> rebuilding_{false};
  // 重建进度（供 stats 轮询）：扫描中的文件/目录计数
  std::atomic<uint64_t> scanFiles_{0};
  std::atomic<uint64_t> scanDirs_{0};

  // 并发契约（Spec 015）——锁序（务必按此顺序获取，禁止反向）：
  //   rebuildM_（重建生命周期，叶子锁：持有时不得再取下列任何锁）
  //     -> maintM_（watcher 生命周期 + cfg_ 读写）
  //       -> dbM_（唯一 SQLite 连接；sqlite3 单连接非线程安全）
  //         -> idxLock_（内存索引；搜索共享 / 变更独占）
  // applyWatch 运行在 watcher 事件线程，只取 dbM_/idxLock_，绝不取 maintM_：
  // 否则 restartWatcherLocked 持 maintM_ 且 join 事件线程时会自锁。
  std::mutex rebuildM_;
  std::condition_variable rebuildCv_;
  std::atomic<bool> rebuildCancel_{false};
  std::thread rebuildThread_;  // joinable，shutdown 前必须 join（不得 detach 越过生命周期）
  std::mutex maintM_;
  std::mutex dbM_;

  // 活动连接跟踪（确保 shutdown 时不产生悬空引用 / 不泄漏线程）
  std::mutex connM_;
  std::vector<int> connFds_;
  std::vector<std::thread> connThreads_;
};

}  // namespace lsearch
