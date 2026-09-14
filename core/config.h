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
  // 热索引模式（Spec 017）：false=memory（常驻精简索引，默认，低延迟）；
  // true=sqlite（不常驻全量条目，查询走 DB，显著降内存、延迟见规格）。
  // 仅启动时读取；切换需改配置并重启（set-opts 不涉及，避免协议面扩张）。
  bool hot_index_sqlite = false;

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

  // 单一"是否索引"判定（Spec 016）：excludes 命中，或（隐藏名 && !index_hidden）时跳过。
  // 全量扫描与 inotify 各事件路径必须共用此判定，避免分支语义漂移。
  bool shouldIndexName(const std::string& name) const;
  bool shouldIndexPath(const std::string& path) const;

  static std::string defaultConfigFile();
  static std::string defaultDbFile();
  static std::string defaultSockFile();
  static std::string defaultLogFile();
};

}  // namespace lsearch
