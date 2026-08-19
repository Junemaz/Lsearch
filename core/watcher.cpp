#include "core/watcher.h"

#include "core/util.h"

#include <dirent.h>
#include <poll.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstring>
#include <string>
#include <vector>

namespace lsearch {

Watcher::~Watcher() { stop(); }

bool Watcher::addWatch(const std::string& path) {
  if (pathToWd_.count(path)) return true;
  int wd = inotify_add_watch(fd_, path.c_str(),
                             IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO |
                                 IN_MODIFY | IN_ATTRIB | IN_DELETE_SELF | IN_MOVE_SELF);
  if (wd < 0) {
    if (errno == ENOSPC && !warnedLimit_) {
      warnedLimit_ = true;
      fprintf(stderr,
              "[lsearchd] inotify watch 已满，增量监控不完整。"
              "可调高 /proc/sys/fs/inotify/max_user_watches。\n");
    }
    return false;
  }
  wdToPath_[wd] = path;
  pathToWd_[path] = wd;
  return true;
}

void Watcher::emitAdded(const std::string& path) {
  if (cfg_.isExcluded(path)) return;
  struct stat st;
  int rc = cfg_.follow_symlinks ? stat(path.c_str(), &st) : lstat(path.c_str(), &st);
  if (rc != 0) return;  // 已消失

  const bool isLink = S_ISLNK(st.st_mode);
  const bool isDir = S_ISDIR(st.st_mode) && !isLink;

  WatchEvent ev;
  ev.type = WatchEvent::Added;
  ev.entry.path = path;
  ev.entry.name = baseName(path);
  ev.entry.size = static_cast<int64_t>(st.st_size);
  ev.entry.mtime = static_cast<int64_t>(st.st_mtime);
  ev.entry.inode = static_cast<uint64_t>(st.st_ino);
  ev.entry.is_dir = isDir;
  if (cb_) cb_(ev);

  if (isDir) scanNewDir(path);
}

void Watcher::emitRemoved(const std::string& path) {
  WatchEvent ev;
  ev.type = WatchEvent::Removed;
  ev.path = path;
  if (cb_) cb_(ev);
}

void Watcher::scanNewDir(const std::string& path) {
  if (cfg_.isExcluded(path)) return;
  if (!addWatch(path)) return;
  DIR* dp = opendir(path.c_str());
  if (!dp) return;
  std::vector<std::string> children;
  struct dirent* de;
  while ((de = readdir(dp)) != nullptr) {
    if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
    if (!cfg_.index_hidden && cfg_.isHiddenName(de->d_name)) continue;
    children.push_back(joinPath(path, de->d_name));
  }
  closedir(dp);
  for (auto& c : children) emitAdded(c);
}

void Watcher::loop() {
  std::vector<char> buf(4096);
  while (!stop_.load()) {
    struct pollfd pfd{fd_, POLLIN, 0};
    int r = poll(&pfd, 1, 500);
    if (r < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if (r == 0) continue;
    ssize_t n = read(fd_, buf.data(), buf.size());
    if (n <= 0) {
      if (n < 0 && (errno == EAGAIN || errno == EINTR)) continue;
      if (n == 0) continue;
      break;
    }
    size_t off = 0;
    while (off < static_cast<size_t>(n)) {
      auto* ev = reinterpret_cast<struct inotify_event*>(buf.data() + off);
      off += sizeof(struct inotify_event) + ev->len;

      auto it = wdToPath_.find(ev->wd);
      if (it == wdToPath_.end()) continue;
      const std::string& dir = it->second;
      const std::string name = ev->len ? std::string(ev->name) : std::string();
      const std::string full = name.empty() ? dir : joinPath(dir, name);

      const uint32_t mask = ev->mask;
      if (mask & (IN_IGNORED | IN_DELETE_SELF | IN_MOVE_SELF)) {
        pathToWd_.erase(dir);
        wdToPath_.erase(ev->wd);
        emitRemoved(full.empty() ? dir : full);
        continue;
      }
      if (mask & (IN_CREATE | IN_MOVED_TO)) {
        if (!cfg_.index_hidden && cfg_.isHiddenName(name)) continue;
        emitAdded(full);
      } else if (mask & (IN_DELETE | IN_MOVED_FROM)) {
        emitRemoved(full);
      } else if (mask & (IN_MODIFY | IN_ATTRIB)) {
        // 仅对现存条目做更新
        WatchEvent w;
        struct stat st;
        int rc = cfg_.follow_symlinks ? stat(full.c_str(), &st) : lstat(full.c_str(), &st);
        if (rc != 0) continue;
        w.type = WatchEvent::Modified;
        w.entry.path = full;
        w.entry.name = baseName(full);
        w.entry.size = static_cast<int64_t>(st.st_size);
        w.entry.mtime = static_cast<int64_t>(st.st_mtime);
        w.entry.inode = static_cast<uint64_t>(st.st_ino);
        w.entry.is_dir = S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode);
        if (cb_) cb_(w);
      }
    }
  }
  running_ = false;
}

bool Watcher::start(const std::vector<std::string>& dirs, const Config& cfg, Cb cb,
                    std::string& err) {
  cfg_ = cfg;
  cb_ = std::move(cb);
  fd_ = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
  if (fd_ < 0) {
    err = "inotify_init failed: " + std::string(strerror(errno));
    return false;
  }
  for (const auto& d : dirs) {
    if (cfg_.isExcluded(d)) continue;
    if (cfg_.index_hidden && cfg_.isHiddenName(baseName(d))) continue;
    addWatch(d);
  }
  stop_ = false;
  running_ = true;
  th_ = std::thread([this] { loop(); });
  return true;
}

void Watcher::stop() {
  if (stop_.exchange(true)) {
    if (th_.joinable()) th_.join();
  }
  if (th_.joinable()) th_.join();
  if (fd_ >= 0) {
    close(fd_);
    fd_ = -1;
  }
}

}  // namespace lsearch
