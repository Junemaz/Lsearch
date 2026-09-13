#include "core/config.h"
#include "core/util.h"
#include "daemon/daemon.h"
#include "ipc/client.h"

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

using namespace lsearch;

// 单例锁：并发冷启动时防止出现两个守护进程（互相偷 socket、同写一个 DB）。
// busy=true 表示锁被其他实例占用（正在启动/运行）；其他失败写 err。
// 返回的持锁 fd 需保持打开至进程退出（flock 随 fd 关闭自动释放，无陈旧锁问题）。
static int acquireInstanceLock(const std::string& sockPath, bool& busy, std::string& err) {
  busy = false;
  size_t slash = sockPath.find_last_of('/');
  if (slash != std::string::npos) mkdir(sockPath.substr(0, slash).c_str(), 0700);
  const std::string lockPath = sockPath + ".lock";
  int fd = ::open(lockPath.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
  if (fd < 0) {
    err = "open " + lockPath + ": " + std::string(strerror(errno));
    return -1;
  }
  if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
    busy = true;
    ::close(fd);
    return -1;
  }
  return fd;
}

static void printUsage(FILE* f) {
  fprintf(f,
          "lsearchd — Lsearch 索引守护进程\n"
          "用法: lsearchd [选项]\n"
          "  --config FILE      指定配置文件 (默认 ~/.config/lsearch/lsearch.conf)\n"
          "  --foreground       前台运行（调试用，日志输出到 stderr）\n"
          "  --rebuild          启动后强制全量重建索引\n"
          "  --add-path P       添加索引根路径 P（已运行则转发命令）\n"
          "  --remove-path P    移除索引根路径 P\n"
          "  --shutdown         停止正在运行的守护进程\n"
          "  --version          显示版本\n"
          "  -h, --help         显示帮助\n");
}

int main(int argc, char** argv) {
  std::string configFile;
  bool foreground = false;
  bool rebuild = false;
  std::string addP, removeP;
  bool doShutdown = false;

  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto next = [&]() -> std::string { return (i + 1 < argc) ? argv[++i] : std::string(); };
    if (a == "--config") {
      configFile = next();
    } else if (a == "--foreground") {
      foreground = true;
    } else if (a == "--rebuild") {
      rebuild = true;
    } else if (a == "--add-path") {
      addP = next();
    } else if (a == "--remove-path") {
      removeP = next();
    } else if (a == "--shutdown") {
      doShutdown = true;
    } else if (a == "--version") {
      printf("lsearchd %s\n", kVersion);
      return 0;
    } else if (a == "-h" || a == "--help") {
      printUsage(stdout);
      return 0;
    } else {
      fprintf(stderr, "未知参数: %s\n", a.c_str());
      printUsage(stderr);
      return 2;
    }
  }

  Config cfg = Config::load(configFile);

  // 首次运行：生成默认配置文件
  struct stat st;
  bool cfgExists = (stat(cfg.config_file.c_str(), &st) == 0);
  if (!cfgExists || !addP.empty() || !removeP.empty()) {
    if (!addP.empty()) {
      bool has = false;
      for (const auto& p : cfg.paths)
        if (p == addP) { has = true; break; }
      if (!has) cfg.paths.push_back(addP);
    }
    if (!removeP.empty()) {
      cfg.paths.erase(std::remove(cfg.paths.begin(), cfg.paths.end(), removeP), cfg.paths.end());
    }
    cfg.save(cfg.config_file);
  }

  if (doShutdown) {
    Client c;
    std::string e;
    if (!c.connect(cfg.sock_path, e)) {
      fprintf(stderr, "没有正在运行的守护进程: %s\n", e.c_str());
      return 1;
    }
    if (!c.command("shutdown", e)) {
      fprintf(stderr, "shutdown 失败: %s\n", e.c_str());
      return 1;
    }
    return 0;
  }

  // 若已有实例在运行，转发命令后退出（避免重复建索引）
  {
    Client probe;
    std::string e;
    bool alive = probe.connect(cfg.sock_path, e);
    if (alive) {
      if (rebuild) probe.command("rebuild", e);
      if (!addP.empty()) probe.command("add-path " + addP, e);
      if (!removeP.empty()) probe.command("remove-path " + removeP, e);
      fprintf(stderr, "lsearchd 已在运行（命令已转发）。\n");
      return 0;
    }
  }

  // 单例锁：探测与真正 bind 之间仍有竞态窗口；加锁确保并发冷启动只产生一个守护进程。
  // 锁 fd 由 fork 后的子进程继承（父进程退出不释放），daemon 退出时自动释放。
  {
    bool busy = false;
    std::string lerr;
    int lockFd = acquireInstanceLock(cfg.sock_path, busy, lerr);
    if (lockFd < 0) {
      if (busy) {
        fprintf(stderr, "lsearchd 另一实例正在启动或运行（单例锁被占用），本进程退出。\n");
        return 0;
      }
      fprintf(stderr, "无法获取单例锁: %s\n", lerr.c_str());
      return 1;
    }
    (void)lockFd;  // 持锁 fd 需保持打开直到进程退出，不可关闭
  }

  // 无运行实例时：--rebuild 需在启动/fork 前强制全量重建（Daemon::init 读取该环境变量）
  if (rebuild) setenv("LSEARCH_FORCE_RESCAN", "1", 1);

  // 守护化（非前台）
  if (!foreground) {
    pid_t pid = fork();
    if (pid < 0) {
      perror("fork");
      return 1;
    }
    if (pid > 0) return 0;  // 父进程退出
    setsid();
    if (chdir("/") != 0) { /* 忽略 */ }

    int dn = open("/dev/null", O_RDWR);
    if (dn >= 0) dup2(dn, 0);
    int lg = open(cfg.log_path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (lg >= 0) {
      dup2(lg, 1);
      dup2(lg, 2);
    } else if (dn >= 0) {
      dup2(dn, 1);
      dup2(dn, 2);
    }
  }

  Daemon daemon(std::move(cfg));
  std::string err;
  if (!daemon.init(err)) {
    fprintf(stderr, "lsearchd 初始化失败: %s\n", err.c_str());
    return 1;
  }
  if (!daemon.run(err)) {
    fprintf(stderr, "lsearchd 运行失败: %s\n", err.c_str());
    return 1;
  }
  return 0;
}
