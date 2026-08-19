#pragma once
// 核心数据结构：文件索引条目与搜索结果
#include <cstdint>
#include <string>

namespace lsearch {

struct FileEntry {
  std::string path;   // 完整路径
  std::string name;   // 文件名（basename）
  int64_t size = 0;   // 字节数（目录为 0）
  int64_t mtime = 0;  // 修改时间（Unix 秒）
  bool is_dir = false;
  uint64_t inode = 0;
};

enum class SortKey { Name, Path, Size, Mtime };

// 排序键的文本表示（IPC / CLI 共用）
const char* sortKeyName(SortKey k);
bool sortKeyFromName(const std::string& s, SortKey& out);

struct SearchResult {
  FileEntry entry;
  bool path_matched = false;  // true 表示命中的是完整路径而非文件名
};

}  // namespace lsearch
