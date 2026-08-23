#pragma once
// IPC 客户端：被 CLI / TUI 复用；连接失败时可自动拉起守护进程
#include "core/entry.h"

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
};

}  // namespace lsearch
