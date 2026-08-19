#pragma once
// 简单配置文件：key = value，注释以 # 开头，列表以逗号分隔
// 默认仅索引用户家目录，保持配置极简。
#include <string>
#include <vector>

namespace lsearch {

struct Config {
  std::vector<std::string> paths;     // 索引根路径
  std::vector<std::string> excludes;  // 排除路径前缀
  bool index_hidden = false;          // 是否索引以 . 开头的隐藏文件/目录
  bool follow_symlinks = false;       // 是否跟随符号链接

  // 派生路径（由环境决定，不入配置）
  std::string config_file;  // 实际加载的配置文件
  std::string db_path;      // SQLite 数据库路径
  std::string sock_path;    // Unix socket 路径
  std::string log_path;     // 守护进程日志路径

  static Config defaults();
  static Config load(const std::string& file);
  void save(const std::string& file) const;

  bool containsPath(const std::string& p) const;
  bool isExcluded(const std::string& path) const;
  bool isHiddenName(const std::string& name) const;

  static std::string defaultConfigFile();
  static std::string defaultDbFile();
  static std::string defaultSockFile();
  static std::string defaultLogFile();
};

}  // namespace lsearch
