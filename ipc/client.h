#pragma once
// IPC 客户端：被 CLI / TUI 复用；连接失败时可自动拉起守护进程
#include "core/entry.h"
#include "core/search.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace lsearch {

class Client {
 public:
  ~Client();
  Client() = default;
  Client(const Client&) = delete;
  Client& operator=(const Client&) = delete;

  bool connect(const std::string& sock, std::string& err);
  void close();
  bool connected() const { return fd_ >= 0; }

  // 单行 OK/ERR 命令
  bool command(const std::string& req, std::string& err);

  // search：结果写 out，total 返回命中总数（可能多于返回条数）
  bool search(const std::string& query, SortKey sort, size_t limit,
              bool dirs_only, bool files_only,
              std::vector<SearchResult>& out, size_t* total, std::string& err);

  // search2：精确 total/total_capped + under 子树过滤；旧 daemon 无 search2 时
  // 自动降级到 search（此时 usedLegacySearch()==true，total 为旧语义）。
  bool searchEx(const std::string& query, SortKey sort, size_t limit,
                bool dirs_only, bool files_only, const std::string& under,
                SearchOutcome& out, std::string& err);

  // count2：精确计数（不物化）；旧 daemon 降级为 legacy search(limit=0) 的截断计数。
  bool countEx(const std::string& query, bool dirs_only, bool files_only,
               const std::string& under, uint64_t& total, bool& capped, std::string& err);

  // capabilities：返回 daemon 支持的命令名列表。
  bool capabilities(std::vector<std::string>& commands, std::string& err);
  // 是否支持 search2/count2（惰性经 capabilities 探测；旧 daemon → false）。
  bool supportsV2();
  // 最近一次 searchEx 是否走了 legacy 降级路径。
  bool usedLegacySearch() const { return lastSearchLegacy_; }

  // stats：返回 key=value 列表
  bool stats(std::vector<std::pair<std::string, std::string>>& kv, std::string& err);

  // 索引管理
  // get-config：返回 paths/excludes/hidden/follow/config_file 的 key=value 列表
  bool getConfig(std::vector<std::pair<std::string, std::string>>& kv, std::string& err);
  bool setPaths(const std::string& pathsCsv, std::string& err);      // 整体替换根路径
  bool setExcludes(const std::string& excludesCsv, std::string& err);  // "_" 清空
  bool setOpts(const std::string& hidden, const std::string& follow, std::string& err);

  // 连接，若失败且 spawn 为 true 则自动拉起守护进程并重试
  static bool connectOrSpawn(const std::string& sock, bool spawn, Client& c, std::string& err);

 private:
  bool writeLine(const std::string& line, std::string& err);
  bool readLine(std::string& line, bool& eof, std::string& err);
  bool ping(std::string& err);
  // 发送请求并解析 "OK\n key=value... END" 回复
  bool readKvReply(const std::string& req, std::vector<std::pair<std::string, std::string>>& kv,
                   std::string& err);

  int fd_ = -1;
  std::string recvBuf_;
  int v2_ = -1;                 // -1 未知 / 0 不支持 / 1 支持 search2
  bool lastSearchLegacy_ = false;
};

}  // namespace lsearch
