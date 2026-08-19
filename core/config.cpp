#include "core/config.h"

#include "core/util.h"

#include <fstream>
#include <set>

namespace lsearch {

Config Config::defaults() {
  Config c;
  std::string home = homeDir();
  c.paths = {home};
  // 系统伪目录 + 常见缓存，避免无意义的索引膨胀（均可配置）
  c.excludes = {"/proc", "/sys", "/dev", "/run", "/snap"};
  if (!home.empty()) {
    c.excludes.push_back(home + "/.cache");
    c.excludes.push_back(home + "/.local/share/Trash");
  }
  c.index_hidden = false;
  c.follow_symlinks = false;
  return c;
}

std::string Config::defaultConfigFile() {
  return joinPath(configDir(), "lsearch.conf");
}

std::string Config::defaultDbFile() {
  return joinPath(dataDir(), "lsearch.db");
}

std::string Config::defaultSockFile() {
  return joinPath(runtimeDir(), "lsearch.sock");
}

std::string Config::defaultLogFile() {
  return joinPath(dataDir(), "lsearchd.log");
}

Config Config::load(const std::string& file) {
  Config c = defaults();
  c.config_file = file.empty() ? defaultConfigFile() : file;
  c.db_path = defaultDbFile();
  c.sock_path = defaultSockFile();
  c.log_path = defaultLogFile();

  std::ifstream in(c.config_file);
  if (!in.is_open()) return c;

  std::string line;
  while (std::getline(in, line)) {
    std::string t = trim(line);
    if (t.empty() || t[0] == '#') continue;
    size_t eq = t.find('=');
    if (eq == std::string::npos) continue;
    std::string key = trim(t.substr(0, eq));
    std::string val = trim(t.substr(eq + 1));
    if (key == "paths") {
      c.paths = split(val, ',');
      if (c.paths.empty()) c.paths.push_back(homeDir());
    } else if (key == "excludes") {
      c.excludes = split(val, ',');
    } else if (key == "index_hidden") {
      c.index_hidden = (val == "1" || val == "true" || val == "yes");
    } else if (key == "follow_symlinks") {
      c.follow_symlinks = (val == "1" || val == "true" || val == "yes");
    }
  }
  return c;
}

void Config::save(const std::string& file) const {
  std::ofstream out(file);
  if (!out.is_open()) return;
  out << "# Lsearch 配置文件（lsearchd 首次运行自动生成）\n";
  out << "# 索引根路径，逗号分隔。默认仅用户家目录，可按需追加，例如：\n";
  out << "#   paths = /home/user,/data\n";
  out << "paths = " << joinList(paths, ",") << "\n";
  out << "\n# 排除的路径前缀，逗号分隔（命中即跳过，含其下所有内容）\n";
  out << "excludes = " << joinList(excludes, ",") << "\n";
  out << "\n# 是否索引以 . 开头的隐藏文件/目录（0/1）\n";
  out << "index_hidden = " << (index_hidden ? "1" : "0") << "\n";
  out << "\n# 是否跟随符号链接（0/1；跟随可避免循环，但不跟随更安全）\n";
  out << "follow_symlinks = " << (follow_symlinks ? "1" : "0") << "\n";
}

bool Config::containsPath(const std::string& p) const {
  for (const auto& r : paths)
    if (r == p || startsWith(p, r + "/")) return true;
  return false;
}

bool Config::isExcluded(const std::string& path) const {
  for (const auto& e : excludes) {
    if (e.empty()) continue;
    if (path == e || startsWith(path, e + "/")) return true;
  }
  return false;
}

bool Config::isHiddenName(const std::string& name) const {
  return !name.empty() && name[0] == '.';
}

}  // namespace lsearch
