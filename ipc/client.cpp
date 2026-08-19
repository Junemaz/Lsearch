#include "ipc/client.h"

#include "core/util.h"
#include "ipc/proto.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdlib>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <thread>

namespace lsearch {

Client::~Client() { close(); }

void Client::close() {
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
  recvBuf_.clear();
}

bool Client::connect(const std::string& sock, std::string& err) {
  if (connected()) {
    // 复用前先测活
    std::string dummy;
    if (ping(dummy)) return true;
    close();
  }
  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) {
    err = "socket: " + std::string(strerror(errno));
    return false;
  }
  struct sockaddr_un addr;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  if (sock.size() >= sizeof(addr.sun_path)) {
    err = "socket path too long";
    ::close(fd);
    return false;
  }
  strncpy(addr.sun_path, sock.c_str(), sizeof(addr.sun_path) - 1);

  struct timeval tv{0, proto::kConnectTimeoutMs * 1000};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

  if (::connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0) {
    err = "connect: " + std::string(strerror(errno));
    ::close(fd);
    return false;
  }
  fd_ = fd;
  std::string pong;
  if (!ping(pong)) {
    err = "daemon on socket not responding";
    close();
    return false;
  }
  return true;
}

bool Client::writeLine(const std::string& line, std::string& err) {
  std::string msg = line + "\n";
  size_t sent = 0;
  while (sent < msg.size()) {
    ssize_t n = ::send(fd_, msg.data() + sent, msg.size() - sent, MSG_NOSIGNAL);
    if (n < 0) {
      err = "send: " + std::string(strerror(errno));
      return false;
    }
    sent += static_cast<size_t>(n);
  }
  return true;
}

bool Client::readLine(std::string& line, bool& eof, std::string& err) {
  line.clear();
  while (true) {
    size_t nl = recvBuf_.find('\n');
    if (nl != std::string::npos) {
      line = recvBuf_.substr(0, nl);
      recvBuf_.erase(0, nl + 1);
      return true;
    }
    char buf[4096];
    ssize_t n = ::recv(fd_, buf, sizeof(buf), 0);
    if (n > 0) {
      recvBuf_.append(buf, static_cast<size_t>(n));
      continue;
    }
    if (n == 0) {
      eof = true;
      return false;
    }
    if (errno == EINTR) continue;
    err = "recv: " + std::string(strerror(errno));
    return false;
  }
}

bool Client::ping(std::string& err) {
  err.clear();
  if (!writeLine("ping", err)) return false;
  std::string line;
  bool eof = false;
  if (!readLine(line, eof, err)) return false;
  return line == "OK pong";
}

bool Client::command(const std::string& req, std::string& err) {
  if (!writeLine(req, err)) return false;
  std::string line;
  bool eof = false;
  if (!readLine(line, eof, err)) return false;
  if (startsWith(line, "OK")) return true;
  err = line;  // "ERR <msg>"
  return false;
}

bool Client::search(const std::string& query, SortKey sort, size_t limit,
                    bool dirs_only, bool files_only,
                    std::vector<SearchResult>& out, size_t* total, std::string& err) {
  out.clear();
  std::string req = "search " + std::to_string(limit) + " " + (dirs_only ? "1" : "0") + " " +
                    (files_only ? "1" : "0") + " " + sortKeyName(sort) + " " + query;
  if (!writeLine(req, err)) return false;

  std::string line;
  bool eof = false;
  if (!readLine(line, eof, err)) return false;
  if (!startsWith(line, "OK")) {
    err = line;
    return false;
  }
  size_t count = 0;
  size_t slash = line.find(' ');
  if (slash != std::string::npos) count = static_cast<size_t>(atoll(line.c_str() + slash + 1));
  if (total) *total = count;

  while (true) {
    if (!readLine(line, eof, err)) return false;
    if (line == "END") break;
    bool is_dir, pm;
    long long size, mtime;
    std::string path;
    if (proto::parseResultLine(line, is_dir, size, mtime, pm, path)) {
      SearchResult r;
      r.entry.path = path;
      r.entry.name = baseName(path);
      r.entry.is_dir = is_dir;
      r.entry.size = size;
      r.entry.mtime = mtime;
      r.path_matched = pm;
      out.push_back(std::move(r));
    }
  }
  return true;
}

bool Client::stats(std::vector<std::pair<std::string, std::string>>& kv, std::string& err) {
  kv.clear();
  if (!writeLine("stats", err)) return false;
  std::string line;
  bool eof = false;
  if (!readLine(line, eof, err)) return false;
  if (!startsWith(line, "OK")) {
    err = line;
    return false;
  }
  while (true) {
    if (!readLine(line, eof, err)) return false;
    if (line == "END") break;
    size_t eq = line.find('=');
    if (eq == std::string::npos) continue;
    kv.emplace_back(line.substr(0, eq), line.substr(eq + 1));
  }
  return true;
}

bool Client::connectOrSpawn(const std::string& sock, bool spawn, Client& c, std::string& err) {
  if (c.connect(sock, err)) return true;
  if (!spawn) return false;

  // 拉起守护进程
  pid_t pid = fork();
  if (pid == 0) {
    setsid();
    int dn = open("/dev/null", O_RDWR);
    if (dn >= 0) {
      dup2(dn, 0);
      dup2(dn, 1);
      dup2(dn, 2);
    }
    if (chdir("/") != 0) { /* 忽略 */ }
    execlp("lsearchd", "lsearchd", (char*)nullptr);
    _exit(127);
  }
  if (pid > 0) {
    int status;
    waitpid(pid, &status, 0);
    for (int i = 0; i < 100; ++i) {
      std::string e2;
      if (c.connect(sock, e2)) return true;
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  }
  err = "daemon did not become ready";
  return false;
}

}  // namespace lsearch
