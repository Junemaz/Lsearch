#include "ipc/client.h"

#include "core/util.h"
#include "ipc/proto.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
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
  v2_ = -1;
  lastSearchLegacy_ = false;
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

bool Client::searchEx(const std::string& query, SortKey sort, size_t limit,
                      bool dirs_only, bool files_only, const std::string& under,
                      SearchOutcome& out, std::string& err) {
  out.results.clear();
  out.total = 0;
  out.total_capped = false;
  lastSearchLegacy_ = false;

  const std::string under64 = under.empty() ? "-" : base64Encode(under);
  const std::string req = "search2 " + std::to_string(limit) + " " + (dirs_only ? "1" : "0") +
                          " " + (files_only ? "1" : "0") + " " + sortKeyName(sort) + " " +
                          under64 + " " + query;
  if (!writeLine(req, err)) return false;

  std::string line;
  bool eof = false;
  if (!readLine(line, eof, err)) return false;
  if (line == "ERR unknown command") {
    v2_ = 0;
    lastSearchLegacy_ = true;
    std::vector<SearchResult> res;
    size_t total = 0;
    if (!search(query, sort, limit, dirs_only, files_only, res, &total, err)) return false;
    out.results = std::move(res);
    out.total = total;
    out.total_capped = false;
    return true;
  }
  if (!startsWith(line, "OK")) {
    err = line;
    return false;
  }
  unsigned long long returned = 0, total = 0;
  int capped = 0;
  if (sscanf(line.c_str(), "OK %llu %llu %d", &returned, &total, &capped) != 3) {
    err = "bad search2 reply: " + line;
    return false;
  }
  (void)returned;
  out.total = static_cast<uint64_t>(total);
  out.total_capped = (capped != 0);

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
      out.results.push_back(std::move(r));
    }
  }
  return true;
}

bool Client::countEx(const std::string& query, bool dirs_only, bool files_only,
                     const std::string& under, uint64_t& total, bool& capped,
                     std::string& err) {
  total = 0;
  capped = false;
  const std::string under64 = under.empty() ? "-" : base64Encode(under);
  const std::string req = "count2 " + std::string(dirs_only ? "1" : "0") + " " +
                          (files_only ? "1" : "0") + " " + under64 + " " + query;
  if (!writeLine(req, err)) return false;

  std::string line;
  bool eof = false;
  if (!readLine(line, eof, err)) return false;
  if (line == "ERR unknown command") {
    v2_ = 0;
    std::vector<SearchResult> res;
    size_t n = 0;
    if (!search(query, SortKey::Name, 0, dirs_only, files_only, res, &n, err)) return false;
    total = n;
    capped = n >= Index::kDefaultCandidateCap;
    return true;
  }
  if (!startsWith(line, "OK")) {
    err = line;
    return false;
  }
  unsigned long long t = 0;
  int c = 0;
  if (sscanf(line.c_str(), "OK %llu %d", &t, &c) != 2) {
    err = "bad count2 reply: " + line;
    return false;
  }
  total = t;
  capped = (c != 0);
  return true;
}

bool Client::capabilities(std::vector<std::string>& commands, std::string& err) {
  commands.clear();
  std::vector<std::pair<std::string, std::string>> kv;
  if (!readKvReply("capabilities", kv, err)) return false;
  for (const auto& e : kv) {
    if (e.first == "commands") {
      commands = split(e.second, ',');
      break;
    }
  }
  return true;
}

bool Client::supportsV2() {
  if (v2_ >= 0) return v2_ == 1;
  std::vector<std::string> cmds;
  std::string err;
  v2_ = 0;
  if (capabilities(cmds, err)) {
    for (const auto& c : cmds) {
      if (c == "search2") {
        v2_ = 1;
        break;
      }
    }
  }
  return v2_ == 1;
}

bool Client::stats(std::vector<std::pair<std::string, std::string>>& kv, std::string& err) {
  return readKvReply("stats", kv, err);
}

bool Client::getConfig(std::vector<std::pair<std::string, std::string>>& kv, std::string& err) {
  return readKvReply("get-config", kv, err);
}

bool Client::readKvReply(const std::string& req,
                         std::vector<std::pair<std::string, std::string>>& kv, std::string& err) {
  kv.clear();
  if (!writeLine(req, err)) return false;
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

bool Client::setPaths(const std::string& pathsCsv, std::string& err) {
  return command("set-paths " + pathsCsv, err);
}

bool Client::setExcludes(const std::string& excludesCsv, std::string& err) {
  return command("set-excludes " + excludesCsv, err);
}

bool Client::setOpts(const std::string& hidden, const std::string& follow, std::string& err) {
  return command("set-opts " + hidden + " " + follow, err);
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
