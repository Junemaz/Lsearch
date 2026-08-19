#pragma once
// 内存热索引：守护进程常驻，支持"即输即搜"
// 语义对齐 Everything 默认行为：大小写不敏感的子串匹配（文件名 + 完整路径），
// 查询含 * 或 ? 时切换为 glob 匹配。提供按名称/路径/大小/修改时间排序。
#include "core/entry.h"

#include <atomic>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace lsearch {

class Index {
 public:
  // 全量重建（丢弃旧索引）
  void build(std::vector<FileEntry>&& entries);

  // 增量：单条增删（路径唯一）
  void add(const FileEntry& e);
  bool remove(const std::string& path);
  // 改名：旧路径 -> 新条目（先删旧索引再插入新条目）
  void replace(const std::string& oldPath, const FileEntry& e);

  // 删除 path 及其所有后代（用于目录移除/移除根路径），返回删除条数
  size_t removePathAndSubtree(const std::string& path);

  // 收集所有目录路径（用于重建 inotify 监视列表）
  void collectDirs(std::vector<std::string>& out) const;

  void clear();

  size_t size() const { return entries_.size(); }
  uint64_t countDirs() const { return dirs_; }
  uint64_t totalBytes() const { return bytes_; }

  // 搜索；limit==0 表示不限（仍受内部候选上限保护）。
  // 结果已按 sort 排序并裁剪到 limit。
  void search(const std::string& query, SortKey sort, size_t limit,
              bool dirs_only, bool files_only,
              std::vector<SearchResult>& out, bool& truncated) const;

 private:
  struct Entry {
    FileEntry e;
    std::string name_low;  // 预计算小写，加快查询
    std::string path_low;
  };

  std::vector<Entry> entries_;
  std::unordered_map<std::string, size_t> pathIndex_;
  std::atomic<uint64_t> dirs_{0};
  std::atomic<uint64_t> bytes_{0};

  static constexpr size_t kCandidateCap = 50000;  // 结果候选上限，避免病态查询拖慢
};

}  // namespace lsearch
