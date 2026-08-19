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

  // 连接，若失败且 spawn 为 true 则自动拉起守护进程并重试
  static bool connectOrSpawn(const std::string& sock, bool spawn, Client& c, std::string& err);

 private:
  bool writeLine(const std::string& line, std::string& err);
  bool readLine(std::string& line, bool& eof, std::string& err);
  bool ping(std::string& err);

  int fd_ = -1;
  std::string recvBuf_;
};

}  // namespace lsearch
