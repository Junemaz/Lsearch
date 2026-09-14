#include "core/indexer.h"

#include "core/util.h"

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <condition_variable>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <set>

namespace lsearch {

// 测试钩子（Spec 015）：LSEARCH_SCAN_DELAY_US>0 时，每处理完一个目录额外 sleep
// 该微秒数，让重建在快机上可控地变慢，从而稳定复现/回归"重建 × 管理命令"竞态。
// 只影响耗时，不影响扫描结果；进程内只读取一次。生产环境未设置 = 0，无行为变化。
static unsigned long scanDelayUs() {
  static const unsigned long v = [] {
    const char* s = getenv("LSEARCH_SCAN_DELAY_US");
    if (!s || !*s) return 0ul;
    long n = atol(s);
    return n > 0 ? static_cast<unsigned long>(n) : 0ul;
  }();
  return v;
}

void fullScan(const Config& cfg, std::vector<FileEntry>& out, ScanProgress prog,
              const std::atomic<bool>* cancel) {
  out.clear();

  auto canceled = [cancel] {
    return cancel != nullptr && cancel->load(std::memory_order_relaxed);
  };
  const unsigned long delayUs = scanDelayUs();

  const unsigned nthreads = [] {
    unsigned n = std::thread::hardware_concurrency();
    if (n < 2) n = 2;
    if (n > 16) n = 16;
    return n;
  }();

  std::mutex queueM;
  std::condition_variable cv;
  std::deque<std::string> queue;
  size_t active = 0;

  auto push = [&](std::string d) {
    std::lock_guard<std::mutex> lk(queueM);
    queue.push_back(std::move(d));
    cv.notify_one();
  };

  auto popClaim = [&](std::string& d) -> bool {
    std::unique_lock<std::mutex> lk(queueM);
    // 用 wait_for 而非 wait：cancel 是 atomic 置位，没有对应的 notify，
    // 阻塞中的 worker 需靠超时轮询及时察觉取消（上限 ~50ms）。
    cv.wait_for(lk, std::chrono::milliseconds(50),
                [&] { return !queue.empty() || active == 0 || canceled(); });
    if (canceled()) return false;
    if (queue.empty()) return false;  // active==0 且空队列 => 结束
    d = std::move(queue.front());
    queue.pop_front();
    ++active;
    return true;
  };

  auto doneDir = [&] {
    std::lock_guard<std::mutex> lk(queueM);
    --active;
    if (active == 0 && queue.empty()) cv.notify_all();
  };

  ScanStats stats;
  std::vector<std::vector<FileEntry>> perThread(nthreads);
  // 符号链接跟随时的目录环检测（dev, ino）
  std::set<std::pair<dev_t, ino_t>> visitedDirs;
  std::mutex visitM;

  auto processDir = [&](const std::string& dir, unsigned tid, std::vector<FileEntry>& local) {
    DIR* dp = opendir(dir.c_str());
    if (!dp) {
      stats.errors.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    std::vector<std::string> subdirs;
    struct dirent* de;
    while ((de = readdir(dp)) != nullptr) {
      if (canceled()) break;
      if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
      std::string child = joinPath(dir, de->d_name);
      if (!cfg.shouldIndexPath(child)) continue;

      struct stat st;
      int rc = cfg.follow_symlinks ? stat(child.c_str(), &st) : lstat(child.c_str(), &st);
      if (rc != 0) {
        stats.errors.fetch_add(1, std::memory_order_relaxed);
        continue;
      }
      const bool isDir = S_ISDIR(st.st_mode);
      const bool isLink = S_ISLNK(st.st_mode);

      FileEntry e;
      e.path = child;
      e.name = de->d_name;
      e.size = static_cast<int64_t>(st.st_size);
      e.mtime = static_cast<int64_t>(st.st_mtime);
      e.inode = static_cast<uint64_t>(st.st_ino);
      e.is_dir = isDir && !isLink;
      local.push_back(std::move(e));

      if (e.is_dir) {
        if (cfg.follow_symlinks) {
          std::lock_guard<std::mutex> lk(visitM);
          auto key = std::make_pair(st.st_dev, st.st_ino);
          if (visitedDirs.insert(key).second)
            subdirs.push_back(child);
        } else if (isDir) {
          subdirs.push_back(child);
        }
      } else {
        stats.files.fetch_add(1, std::memory_order_relaxed);
      }
    }
    closedir(dp);

    for (auto& sub : subdirs) push(std::move(sub));
    if (!subdirs.empty()) stats.dirs.fetch_add(subdirs.size(), std::memory_order_relaxed);
    if (delayUs > 0) usleep(static_cast<useconds_t>(delayUs));
    (void)tid;
  };

  // 先把根路径全部入队，再启动 worker —— 顺序必须如此！否则 worker 可能先看到
  // 空队列+活跃0 而提前退出，导致本轮扫描返回 0 条（间歇性"索引被清空"竞态）。
  for (const auto& r : cfg.paths) {
    if (cfg.isExcluded(r)) continue;
    if (cfg.follow_symlinks) {
      struct stat st;
      if (stat(r.c_str(), &st) == 0)
        visitedDirs.insert(std::make_pair(st.st_dev, st.st_ino));
    }
    push(r);
  }

  std::vector<std::thread> workers;
  workers.reserve(nthreads);
  for (unsigned t = 0; t < nthreads; ++t) {
    workers.emplace_back([&, t] {
      while (true) {
        if (canceled()) break;
        std::string d;
        if (!popClaim(d)) break;
        processDir(d, t, perThread[t]);
        doneDir();
      }
    });
  }

  // 进度回调线程（约每秒一次）
  std::atomic<bool> scanDone{false};
  std::thread progressThread;
  if (prog) {
    progressThread = std::thread([&] {
      while (!scanDone.load(std::memory_order_relaxed)) {
        usleep(1000 * 1000);
        if (prog) prog(stats);
      }
    });
  }

  for (auto& th : workers) th.join();
  scanDone.store(true, std::memory_order_relaxed);
  if (progressThread.joinable()) progressThread.join();
  if (prog) prog(stats);

  size_t total = 0;
  for (auto& v : perThread) total += v.size();
  out.reserve(total);
  for (auto& v : perThread)
    for (auto& e : v) out.push_back(std::move(e));
}

}  // namespace lsearch
