#include "core/config.h"
#include "core/entry.h"
#include "core/util.h"
#include "ipc/client.h"
#include "ipc/proto.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace lsearch;

static void printUsage(FILE* f) {
  fprintf(f,
          "lsearch — 在索引中快速搜索文件名（Everything 风格 Linux 实现）\n"
          "用法: lsearch [选项] <关键词>\n"
          "  -l, --limit N      最多返回 N 条 (默认 200, 0=不限)\n"
          "  -s, --sort KEY     排序: name|path|size|mtime (默认 name)\n"
          "  -c, --count        仅打印命中总数\n"
          "  -d, --dirs-only    只显示目录\n"
          "  -f, --files-only   只显示文件\n"
          "  -S, --details      显示 类型/大小/时间\n"
          "  -0, --print0       以 NUL 分隔输出（脚本友好）\n"
          "  -m, --no-daemon-spawn  不自动拉起守护进程\n"
          "      --stats        显示索引统计\n"
          "      --rebuild      触发重新建索引\n"
          "  -v, --version      显示版本\n"
          "  -h, --help         显示帮助\n"
          "关键词为大小写不敏感的子串匹配；含 * 或 ? 时按通配符匹配。\n");
}

int main(int argc, char** argv) {
  size_t limit = 200;
  SortKey sort = SortKey::Name;
  bool countOnly = false, dirsOnly = false, filesOnly = false, details = false;
  bool print0 = false, noSpawn = false, showStats = false, doRebuild = false;
  std::vector<std::string> positional;

  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto next = [&]() -> std::string { return (i + 1 < argc) ? argv[++i] : std::string(); };
    if (a == "-l" || a == "--limit") {
      limit = static_cast<size_t>(atol(next().c_str()));
    } else if (a == "-s" || a == "--sort") {
      if (!sortKeyFromName(next(), sort)) {
        fprintf(stderr, "非法排序键（应为 name|path|size|mtime）\n");
        return 2;
      }
    } else if (a == "-c" || a == "--count") {
      countOnly = true;
    } else if (a == "-d" || a == "--dirs-only") {
      dirsOnly = true;
    } else if (a == "-f" || a == "--files-only") {
      filesOnly = true;
    } else if (a == "-S" || a == "--details") {
      details = true;
    } else if (a == "-0" || a == "--print0") {
      print0 = true;
    } else if (a == "-m" || a == "--no-daemon-spawn") {
      noSpawn = true;
    } else if (a == "--stats") {
      showStats = true;
    } else if (a == "--rebuild") {
      doRebuild = true;
    } else if (a == "-v" || a == "--version") {
      printf("lsearch 0.1.0\n");
      return 0;
    } else if (a == "-h" || a == "--help") {
      printUsage(stdout);
      return 0;
    } else if (a.size() > 1 && a[0] == '-') {
      fprintf(stderr, "未知选项: %s\n", a.c_str());
      printUsage(stderr);
      return 2;
    } else {
      positional.push_back(a);
    }
  }

  Config cfg = Config::load("");

  Client c;
  std::string err;
  if (!Client::connectOrSpawn(cfg.sock_path, !noSpawn, c, err)) {
    fprintf(stderr, "无法连接守护进程: %s\n（先运行 lsearchd）\n", err.c_str());
    return 2;
  }

  if (showStats) {
    std::vector<std::pair<std::string, std::string>> kv;
    if (!c.stats(kv, err)) {
      fprintf(stderr, "获取统计失败: %s\n", err.c_str());
      return 2;
    }
    for (auto& [k, v] : kv) {
      if (k == "size") printf("size=%s\n", humanSize(atoll(v.c_str())).c_str());
      else if (k == "uptime") printf("uptime=%ss\n", v.c_str());
      else printf("%s=%s\n", k.c_str(), v.c_str());
    }
    return 0;
  }

  if (doRebuild) {
    if (!c.command("rebuild", err)) {
      fprintf(stderr, "触发重建失败: %s\n", err.c_str());
      return 2;
    }
    printf("已触发索引重建（后台执行）。\n");
    return 0;
  }

  std::string query;
  for (const auto& p : positional) {
    if (!query.empty()) query += " ";
    query += p;
  }
  if (query.empty()) {
    fprintf(stderr, "缺少关键词（lsearch --help 查看用法）\n");
    return 2;
  }

  std::vector<SearchResult> out;
  size_t total = 0;
  if (!c.search(query, sort, limit, dirsOnly, filesOnly, out, &total, err)) {
    fprintf(stderr, "搜索失败: %s\n", err.c_str());
    return 2;
  }

  if (countOnly) {
    printf("%zu\n", total);
    return total > 0 ? 0 : 1;
  }
  if (out.empty()) return 1;

  const char sep = print0 ? '\0' : '\n';
  for (const auto& r : out) {
    if (details) {
      printf("%s\t%c\t%s\t%s%c", r.entry.path.c_str(), r.entry.is_dir ? 'D' : 'F',
             humanSize(r.entry.size).c_str(), isoTime(r.entry.mtime).c_str(), '\n');
    } else {
      printf("%s%c", r.entry.path.c_str(), sep);
    }
  }
  return 0;
}
