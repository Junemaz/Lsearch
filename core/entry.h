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

// st_size 合理性上限（Spec 016 F3）：搜索索引里不可能出现 >1 PiB 的条目。
// 伪文件/稀疏文件/损坏 stat 可能报出接近 INT64_MAX 的值，统一按 0 处理，
// 避免 totalBytes() 累加溢出并污染按大小排序。
inline constexpr int64_t kMaxPlausibleFileSize = 1LL << 50;  // 1 PiB
inline int64_t sanitizeSize(int64_t s) {
  return (s < 0 || s > kMaxPlausibleFileSize) ? 0 : s;
}

struct SearchResult {
  FileEntry entry;
  bool path_matched = false;  // true 表示命中的是完整路径而非文件名
};

}  // namespace lsearch
