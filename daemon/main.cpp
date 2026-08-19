#include "core/config.h"
#include "core/util.h"
#include "daemon/daemon.h"
#include "ipc/client.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

using namespace lsearch;

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
