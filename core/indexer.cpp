#include "core/indexer.h"

#include "core/util.h"

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <set>

namespace lsearch {

void fullScan(const Config& cfg, std::vector<FileEntry>& out, ScanProgress prog) {
  out.clear();

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
    cv.wait(lk, [&] { return !queue.empty() || active == 0; });
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
      if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
      std::string child = joinPath(dir, de->d_name);
      if (cfg.isExcluded(child)) continue;
      if (!cfg.index_hidden && cfg.isHiddenName(de->d_name)) continue;

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
    (void)tid;
  };

  std::vector<std::thread> workers;
  workers.reserve(nthreads);
  for (unsigned t = 0; t < nthreads; ++t) {
    workers.emplace_back([&, t] {
      while (true) {
        std::string d;
        if (!popClaim(d)) break;
        processDir(d, t, perThread[t]);
        doneDir();
      }
    });
  }

  // 启动根路径
  for (const auto& r : cfg.paths) {
    if (cfg.isExcluded(r)) continue;
    if (cfg.follow_symlinks) {
      struct stat st;
      if (stat(r.c_str(), &st) == 0)
        visitedDirs.insert(std::make_pair(st.st_dev, st.st_ino));
    }
    push(r);
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
