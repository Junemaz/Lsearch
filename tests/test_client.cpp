#include "test_util.h"

#include "core/search.h"
#include "ipc/client.h"

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <thread>
#include <utility>

using namespace lsearch;

namespace {

// 极简假 daemon：按 handler 逐行应答，用于模拟旧 daemon 与瞬时故障。
class StubServer {
 public:
  using Handler = std::function<std::string(const std::string&)>;

  explicit StubServer(Handler h) : handler_(std::move(h)) {
    char tmpl[] = "/tmp/lsearch-stub-XXXXXX";
    char* dir = mkdtemp(tmpl);
    dir_ = dir ? dir : std::string("/tmp");
    path_ = dir_ + "/s.sock";
    fd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un a;
    std::memset(&a, 0, sizeof(a));
    a.sun_family = AF_UNIX;
    (void)std::strncpy(a.sun_path, path_.c_str(), sizeof(a.sun_path) - 1);
    (void)::bind(fd_, reinterpret_cast<struct sockaddr*>(&a), sizeof(a));
    (void)::listen(fd_, 8);
    thread_ = std::thread([this] { serve(); });
  }

  ~StubServer() {
    stop_ = true;
    if (fd_ >= 0) ::shutdown(fd_, SHUT_RDWR);
    const int wake = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (wake >= 0) {
      struct sockaddr_un a;
      std::memset(&a, 0, sizeof(a));
      a.sun_family = AF_UNIX;
      (void)std::strncpy(a.sun_path, path_.c_str(), sizeof(a.sun_path) - 1);
      (void)::connect(wake, reinterpret_cast<struct sockaddr*>(&a), sizeof(a));
      ::close(wake);
    }
    if (thread_.joinable()) thread_.join();
    if (fd_ >= 0) ::close(fd_);
    ::unlink(path_.c_str());
    ::rmdir(dir_.c_str());
  }

  const std::string& path() const { return path_; }

 private:
  void serve() {
    while (!stop_) {
      const int c = ::accept(fd_, nullptr, nullptr);
      if (c < 0) break;
      serveConn(c);
      ::shutdown(c, SHUT_RDWR);
      ::close(c);
    }
  }

  void serveConn(int c) {
    std::string buf;
    char tmp[1024];
    while (!stop_) {
      const ssize_t n = ::recv(c, tmp, sizeof(tmp), 0);
      if (n <= 0) break;
      buf.append(tmp, static_cast<size_t>(n));
      size_t pos;
      while ((pos = buf.find('\n')) != std::string::npos) {
        const std::string line = buf.substr(0, pos);
        buf.erase(0, pos + 1);
        const std::string resp = handler_(line);
        if (resp.empty()) return;
        size_t sent = 0;
        while (sent < resp.size()) {
          const ssize_t w = ::send(c, resp.data() + sent, resp.size() - sent, MSG_NOSIGNAL);
          if (w <= 0) return;
          sent += static_cast<size_t>(w);
        }
      }
    }
  }

  Handler handler_;
  std::string dir_;
  std::string path_;
  int fd_ = -1;
  std::atomic<bool> stop_{false};
  std::thread thread_;
};

std::string oldDaemonHandler(const std::string& line) {
  if (line == "ping") return "OK pong\n";
  if (line.rfind("search2", 0) == 0) return "ERR unknown command\n";
  if (line.rfind("count2", 0) == 0) return "ERR unknown command\n";
  if (line == "capabilities") return "ERR unknown command\n";
  if (line.rfind("search ", 0) == 0) return "OK 1\n/tmp/stub_hit.txt\t0\t7\t12345\t0\nEND\n";
  return "ERR unknown command\n";
}

bool connectClient(Client& c, const std::string& sock) {
  std::string err;
  return c.connect(sock, err);
}

}  // namespace

TEST(client_search_ex_under_requires_v2) {
  StubServer srv(oldDaemonHandler);
  Client c;
  CHECK(connectClient(c, srv.path()));
  SearchOutcome out;
  std::string err;
  CHECK(!c.searchEx("x", SortKey::Name, 20, false, false, "/data", out, err));
  CHECK(err.find("does not support 'under'") != std::string::npos);
}

TEST(client_search_ex_no_under_legacy_ok) {
  StubServer srv(oldDaemonHandler);
  Client c;
  CHECK(connectClient(c, srv.path()));
  SearchOutcome out;
  std::string err;
  CHECK(c.searchEx("x", SortKey::Name, 20, false, false, "", out, err));
  CHECK(c.usedLegacySearch());
  CHECK_EQ(out.total, (uint64_t)1);
  CHECK_EQ(out.results.size(), (size_t)1);
  CHECK_EQ(out.results[0].entry.path, std::string("/tmp/stub_hit.txt"));
}

TEST(client_count_ex_under_requires_v2) {
  StubServer srv(oldDaemonHandler);
  Client c;
  CHECK(connectClient(c, srv.path()));
  uint64_t total = 0;
  bool capped = false;
  std::string err;
  CHECK(!c.countEx("x", false, false, "/data", total, capped, err));
  CHECK(err.find("does not support 'under'") != std::string::npos);
}

TEST(client_count_ex_no_under_legacy_ok) {
  StubServer srv(oldDaemonHandler);
  Client c;
  CHECK(connectClient(c, srv.path()));
  uint64_t total = 0;
  bool capped = true;
  std::string err;
  CHECK(c.countEx("x", false, false, "", total, capped, err));
  CHECK_EQ(total, (uint64_t)1);
  CHECK(!capped);
}

TEST(client_under_error_after_v2_confirmed_absent) {
  StubServer srv(oldDaemonHandler);
  Client c;
  CHECK(connectClient(c, srv.path()));
  SearchOutcome out;
  std::string err;
  CHECK(c.searchEx("x", SortKey::Name, 20, false, false, "", out, err));
  CHECK(c.usedLegacySearch());

  err.clear();
  CHECK(!c.searchEx("x", SortKey::Name, 20, false, false, "/data", out, err));
  CHECK(err.find("does not support 'under'") != std::string::npos);

  uint64_t total = 0;
  bool capped = false;
  CHECK(!c.countEx("x", false, false, "/data", total, capped, err));
  CHECK(err.find("does not support 'under'") != std::string::npos);
}

TEST(client_supports_v2_definitive_negative_cached) {
  std::atomic<int> caps{0};
  StubServer srv([&](const std::string& line) -> std::string {
    if (line == "ping") return "OK pong\n";
    if (line == "capabilities") {
      ++caps;
      return "OK\ncommands=search,stats\nEND\n";
    }
    return "ERR unknown command\n";
  });
  Client c;
  CHECK(connectClient(c, srv.path()));
  CHECK(!c.supportsV2());
  CHECK(!c.supportsV2());
  CHECK_EQ(caps.load(), 1);  // 确定性结论缓存，不重复探测
}

TEST(client_supports_v2_transient_retry) {
  std::atomic<int> caps{0};
  StubServer srv([&](const std::string& line) -> std::string {
    if (line == "ping") return "OK pong\n";
    if (line == "capabilities") {
      const int n = ++caps;
      if (n == 1) return "ERR temporary\n";
      return "OK\ncommands=search,search2,count2\nEND\n";
    }
    if (line.rfind("search2", 0) == 0) return "OK 1 1 0\n/tmp/x\t0\t1\t1\t0\nEND\n";
    return "ERR unknown command\n";
  });
  Client c;
  CHECK(connectClient(c, srv.path()));
  CHECK(!c.supportsV2());  // 瞬时失败不缓存
  CHECK(c.supportsV2());   // 下次调用重试成功
  CHECK_EQ(caps.load(), 2);
}

TEST(client_supports_v2_positive_search2_count2) {
  StubServer srv([](const std::string& line) -> std::string {
    if (line == "ping") return "OK pong\n";
    if (line == "capabilities") return "OK\ncommands=search,search2,count2\nEND\n";
    if (line.rfind("search2", 0) == 0) return "OK 1 1 0\n/tmp/hit2\t0\t1\t1\t0\nEND\n";
    if (line.rfind("count2", 0) == 0) return "OK 9 1\n";
    return "ERR unknown command\n";
  });
  Client c;
  CHECK(connectClient(c, srv.path()));
  CHECK(c.supportsV2());

  SearchOutcome out;
  std::string err;
  CHECK(c.searchEx("q", SortKey::Name, 20, false, false, "/data", out, err));
  CHECK(!c.usedLegacySearch());
  CHECK_EQ(out.total, (uint64_t)1);
  CHECK(!out.total_capped);
  CHECK_EQ(out.results.size(), (size_t)1);

  uint64_t total = 0;
  bool capped = false;
  CHECK(c.countEx("q", false, false, "/data", total, capped, err));
  CHECK_EQ(total, (uint64_t)9);
  CHECK(capped);
}
