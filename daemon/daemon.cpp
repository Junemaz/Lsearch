#include "daemon/daemon.h"

#include "core/indexer.h"
#include "core/util.h"
#include "ipc/proto.h"

#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <thread>

namespace lsearch {

Daemon::Daemon(Config cfg) : cfg_(std::move(cfg)) {}

Daemon::~Daemon() {
  watcher_.stop();
  if (listenFd_ >= 0) ::close(listenFd_);
}

void Daemon::shutdown() { running_ = false; }

void Daemon::applyWatch(const WatchEvent& ev) {
  switch (ev.type) {
    case WatchEvent::Added:
    case WatchEvent::Modified:
      db_.upsert(ev.entry);
      {
        std::unique_lock<std::shared_mutex> lk(idxLock_);
        idx_.add(ev.entry);
      }
      break;
    case WatchEvent::Removed: {
      std::unique_lock<std::shared_mutex> lk(idxLock_);
      idx_.removePathAndSubtree(ev.path);
      db_.removeSubtree(ev.path);
      break;
    }
  }
}

bool Daemon::doFullScan() {
  scanFiles_ = 0;
  scanDirs_ = 0;
  std::vector<FileEntry> entries;
  fullScan(cfg_, entries, [this](const ScanStats& s) {
    scanFiles_ = s.files.load();
    scanDirs_ = s.dirs.load();
    fprintf(stderr, "[lsearchd] scanning: files=%llu dirs=%llu errors=%llu\n",
            static_cast<unsigned long long>(s.files.load()),
            static_cast<unsigned long long>(s.dirs.load()),
            static_cast<unsigned long long>(s.errors.load()));
  });

  // 防御：根路径非空却扫到 0 条，视为异常，绝不能拿空索引覆盖现有好数据
  bool rootsEmpty = true;
  for (const auto& r : cfg_.paths)
    if (!cfg_.isExcluded(r)) { rootsEmpty = false; break; }
  if (!rootsEmpty && entries.empty()) {
    fprintf(stderr, "[lsearchd] 全量扫描返回 0 条（根路径非空）—— 保留现有索引，跳过本次覆盖\n");
    return false;
  }

  db_.clearFiles();
  db_.begin();
  for (auto& e : entries) db_.upsert(e);
  db_.commit();
  {
    std::unique_lock<std::shared_mutex> lk(idxLock_);
    idx_.build(std::move(entries));
  }
  db_.setMeta("last_scan", std::to_string(nowSeconds()));
  db_.setMeta("entries", std::to_string(idx_.size()));
  fprintf(stderr, "[lsearchd] scan done: %zu entries\n", idx_.size());
  return true;
}

bool Daemon::loadIndexFromDb() {
  std::vector<FileEntry> entries;
  if (!db_.loadAll(entries) || entries.empty()) return false;
  std::unique_lock<std::shared_mutex> lk(idxLock_);
  idx_.build(std::move(entries));
  return idx_.size() > 0;
}

bool Daemon::init(std::string& err) {
  if (!db_.open(cfg_.db_path, err)) return false;
  startedAt_ = nowSeconds();
  if (getenv("LSEARCH_FORCE_RESCAN")) {
    doFullScan();
  } else if (!loadIndexFromDb()) {
    doFullScan();
  }
  return true;
}

void Daemon::restartWatcher() {
  watcher_.stop();
  std::vector<std::string> dirs;
  {
    std::shared_lock<std::shared_mutex> lk(idxLock_);
    idx_.collectDirs(dirs);
  }
  std::string werr;
  if (!watcher_.start(dirs, cfg_, [this](const WatchEvent& ev) { applyWatch(ev); }, werr)) {
    fprintf(stderr, "[lsearchd] inotify 启动失败（仍可手动搜索）: %s\n", werr.c_str());
  } else {
    fprintf(stderr, "[lsearchd] inotify 监控 %zu 个目录\n", dirs.size());
  }
}

void Daemon::rebuildAsync() {
  watcher_.stop();
  doFullScan();
  restartWatcher();
  std::lock_guard<std::mutex> lk(rebuildM_);
  rebuilding_ = false;
}

void Daemon::startRebuild() {
  std::lock_guard<std::mutex> lk(rebuildM_);
  if (rebuilding_) return;
  rebuilding_ = true;
  std::thread([this] { rebuildAsync(); }).detach();
}

void Daemon::buildSearchResponse(const std::string& line, std::string& out) {
  std::vector<std::string> head;
  std::string rest;
  if (!proto::splitHead(line, 5, head, rest)) {
    out = "ERR bad args\n";
    return;
  }
  size_t limit = static_cast<size_t>(atoll(head[1].c_str()));
  bool dirs_only = head[2] == "1";
  bool files_only = head[3] == "1";
  SortKey sort;
  if (!sortKeyFromName(head[4], sort)) {
    out = "ERR bad sort\n";
    return;
  }
  std::vector<SearchResult> res;
  bool truncated = false;
  {
    std::shared_lock<std::shared_mutex> lk(idxLock_);
    idx_.search(rest, sort, limit, dirs_only, files_only, res, truncated);
  }
  out = "OK " + std::to_string(res.size()) + "\n";
  for (const auto& r : res) {
    out += r.entry.path + "\t" + (r.entry.is_dir ? "1" : "0") + "\t" +
           std::to_string(r.entry.size) + "\t" + std::to_string(r.entry.mtime) + "\t" +
           (r.path_matched ? "1" : "0") + "\n";
  }
  out += "END\n";
}

void Daemon::handleRequest(const std::string& line, std::string& out) {
  size_t sp = line.find(' ');
  std::string cmd = (sp == std::string::npos) ? line : line.substr(0, sp);
  std::string arg = (sp == std::string::npos) ? "" : line.substr(sp + 1);

  if (cmd == "ping") {
    out = "OK pong\n";
  } else if (cmd == "version") {
    out = std::string("OK\nversion=") + kVersion + "\nEND\n";
  } else if (cmd == "stats") {
    uint64_t files = 0, dirs = 0, bytes = 0;
    {
      std::shared_lock<std::shared_mutex> lk(idxLock_);
      files = idx_.size();
      dirs = idx_.countDirs();
      bytes = idx_.totalBytes();
    }
    out = "OK\n";
    out += "files=" + std::to_string(files) + "\n";
    out += "dirs=" + std::to_string(dirs) + "\n";
    out += "size=" + std::to_string(bytes) + "\n";
    out += "uptime=" + std::to_string(nowSeconds() - startedAt_) + "\n";
    std::string roots;
    for (const auto& r : cfg_.paths) {
      if (!roots.empty()) roots += ",";
      roots += r;
    }
    out += "roots=" + roots + "\n";
    out += "rebuilding=" + std::string(rebuilding_ ? "1" : "0") + "\n";
    out += "scan_files=" + std::to_string(scanFiles_.load()) + "\n";
    out += "scan_dirs=" + std::to_string(scanDirs_.load()) + "\n";
    out += "END\n";
  } else if (cmd == "search") {
    buildSearchResponse(line, out);
  } else if (cmd == "rebuild") {
    startRebuild();
    out = "OK\n";
  } else if (cmd == "add-path" && !arg.empty()) {
    bool has = false;
    for (const auto& p : cfg_.paths)
      if (p == arg) { has = true; break; }
    if (!has) {
      cfg_.paths.push_back(arg);
      cfg_.save(cfg_.config_file);
    }
    startRebuild();
    out = "OK\n";
  } else if (cmd == "remove-path" && !arg.empty()) {
    cfg_.paths.erase(std::remove(cfg_.paths.begin(), cfg_.paths.end(), arg), cfg_.paths.end());
    {
      std::unique_lock<std::shared_mutex> lk(idxLock_);
      idx_.removePathAndSubtree(arg);
      db_.removeSubtree(arg);
    }
    restartWatcher();
    out = "OK\n";
  } else if (cmd == "get-config") {
    out = "OK\n";
    out += "paths=" + joinList(cfg_.paths, ",") + "\n";
    out += "excludes=" + joinList(cfg_.excludes, ",") + "\n";
    out += "hidden=" + std::string(cfg_.index_hidden ? "1" : "0") + "\n";
    out += "follow=" + std::string(cfg_.follow_symlinks ? "1" : "0") + "\n";
    out += "config_file=" + cfg_.config_file + "\n";
    out += "END\n";
  } else if (cmd == "set-paths") {
    // paths 为空拒绝（至少保留一个根目录），csv 逗号分隔
    auto v = split(arg, ',');
    if (v.empty()) {
      out = "ERR paths must not be empty\n";
    } else {
      cfg_.paths = std::move(v);
      cfg_.save(cfg_.config_file);
      startRebuild();
      out = "OK\n";
    }
  } else if (cmd == "set-excludes") {
    // 传 "_" 表示清空排除列表
    cfg_.excludes = (arg == "_") ? std::vector<std::string>{} : split(arg, ',');
    cfg_.save(cfg_.config_file);
    startRebuild();
    out = "OK\n";
  } else if (cmd == "set-opts") {
    // set-opts <hidden 0|1> <follow 0|1>
    auto v = split(arg, ' ');
    if (v.size() >= 2) {
      cfg_.index_hidden = (v[0] == "1");
      cfg_.follow_symlinks = (v[1] == "1");
      cfg_.save(cfg_.config_file);
      startRebuild();
      out = "OK\n";
    } else {
      out = "ERR bad opts\n";
    }
  } else if (cmd == "shutdown") {
    out = "OK\n";
    shutdown();
  } else {
    out = "ERR unknown command\n";
  }
}

void Daemon::serveConnection(int fd) {
  std::string buf;
  char tmp[4096];
  while (running_.load()) {
    ssize_t n = ::recv(fd, tmp, sizeof(tmp), 0);
    if (n == 0) break;
    if (n < 0) {
      if (errno == EINTR) continue;
      break;
    }
    buf.append(tmp, static_cast<size_t>(n));
    size_t pos;
    while ((pos = buf.find('\n')) != std::string::npos) {
      std::string line = buf.substr(0, pos);
      buf.erase(0, pos + 1);
      if (line.empty()) continue;
      std::string out;
      handleRequest(line, out);
      size_t sent = 0;
      while (sent < out.size()) {
        ssize_t w = ::send(fd, out.data() + sent, out.size() - sent, MSG_NOSIGNAL);
        if (w < 0) {
          if (errno == EINTR) continue;
          goto done;
        }
        sent += static_cast<size_t>(w);
      }
      if (!running_.load()) break;
    }
  }
done:
  {
    std::lock_guard<std::mutex> lk(connM_);
    connFds_.erase(std::remove(connFds_.begin(), connFds_.end(), fd), connFds_.end());
  }
  ::shutdown(fd, SHUT_RDWR);
  ::close(fd);
}

bool Daemon::run(std::string& err) {
  const std::string& sock = cfg_.sock_path;
  size_t slash = sock.find_last_of('/');
  if (slash != std::string::npos) mkdir(sock.substr(0, slash).c_str(), 0700);
  ::unlink(sock.c_str());

  listenFd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (listenFd_ < 0) {
    err = "socket: " + std::string(strerror(errno));
    return false;
  }
  int one = 1;
  ::setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  struct sockaddr_un a;
  memset(&a, 0, sizeof(a));
  a.sun_family = AF_UNIX;
  strncpy(a.sun_path, sock.c_str(), sizeof(a.sun_path) - 1);
  if (::bind(listenFd_, reinterpret_cast<struct sockaddr*>(&a), sizeof(a)) != 0) {
    err = "bind " + sock + ": " + std::string(strerror(errno));
    ::close(listenFd_);
    listenFd_ = -1;
    return false;
  }
  if (::listen(listenFd_, 64) != 0) {
    err = "listen: " + std::string(strerror(errno));
    return false;
  }
  chmod(sock.c_str(), 0600);

  restartWatcher();

  fprintf(stderr, "[lsearchd] %s ready on socket %s\n", kVersion, sock.c_str());

  while (running_.load()) {
    struct pollfd p{listenFd_, POLLIN, 0};
    int r = poll(&p, 1, 500);
    if (r < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if (r == 0) continue;
    int c = accept(listenFd_, nullptr, nullptr);
    if (c < 0) continue;
    {
      std::lock_guard<std::mutex> lk(connM_);
      connFds_.push_back(c);
    }
    connThreads_.emplace_back([this, c] { serveConnection(c); });
  }

  // 关闭 accept 后：唤醒所有仍阻塞在 recv 的连接线程并回收，避免悬空引用
  {
    std::vector<int> toWake;
    {
      std::lock_guard<std::mutex> lk(connM_);
      toWake = connFds_;
    }
    for (int fd : toWake) ::shutdown(fd, SHUT_RDWR);  // recv 返回 0 -> 线程退出
  }
  for (auto& t : connThreads_)
    if (t.joinable()) t.join();

  watcher_.stop();
  ::unlink(sock.c_str());
  return true;
}

}  // namespace lsearch