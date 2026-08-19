#pragma once
// inotify 增量索引：监控被索引目录，产生增删改事件回调，供守护进程实时更新 DB 与内存索引
#include "core/config.h"
#include "core/entry.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace lsearch {

struct WatchEvent {
  enum Type { Added, Removed, Modified } type = Added;
  FileEntry entry;   // Added / Modified 时有效
  std::string path;  // Removed 时有效
};

class Watcher {
 public:
  using Cb = std::function<void(const WatchEvent&)>;

  ~Watcher();

  // dirs: 需要递归监控的所有目录（守护进程由内存索引中的 is_dir 条目构建）。
  // cb 在内部事件线程中调用（不得调用 watch 阻塞）。
  bool start(const std::vector<std::string>& dirs, const Config& cfg, Cb cb, std::string& err);
  void stop();
  bool running() const { return running_; }

 private:
  void loop();
  bool addWatch(const std::string& path);
  void emitAdded(const std::string& path);
  void emitRemoved(const std::string& path);
  void scanNewDir(const std::string& path);

  int fd_ = -1;
  std::atomic<bool> running_{false};
  std::atomic<bool> stop_{false};
  std::thread th_;

  std::unordered_map<int, std::string> wdToPath_;
  std::unordered_map<std::string, int> pathToWd_;
  Cb cb_;
  Config cfg_;
  bool warnedLimit_ = false;
};

}  // namespace lsearch
