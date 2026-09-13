#pragma once
// 内存热索引：守护进程常驻，支持"即输即搜"
// 语义：仅按**最终文件/文件夹名（basename）**做大小写不敏感的子串匹配；
// 查询以 `re:` 开头时切换为 ECMAScript 正则（大小写不敏感，regex_search），
// 否则含 * 或 ? 时切换为 glob 匹配。提供按名称/路径/大小/修改时间排序。
#include "core/entry.h"

#include <atomic>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace lsearch {

// 校验查询字符串：`re:` 前缀且正则非法时返回 false 并把错误写入 err；
// 其余情况（含非 `re:` 查询、`re:` 后为空/全空白）一律返回 true。
// 与 Index::search 使用同一编译路径，避免"校验通过但匹配失败"。
bool validateQuery(const std::string& query, std::string& err);

// 精确检索结果：total_capped==false 时 total 为真实命中数、页间稳定；
// true 时扫描在候选上限处停止，total 为下界（≈cap），结果子集不保证跨调用稳定。
struct SearchOutcome {
  std::vector<SearchResult> results;
  uint64_t total = 0;
  bool total_capped = false;
};

class Index {
 public:
  // 候选上限默认值（daemon / MCP 降级路径共用同一来源）
  static constexpr size_t kDefaultCandidateCap = 50000;

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
  // 早停语义（依赖快速路径的 TUI/GUI/旧 search 命令）：候选数达到 limit 即停止扫描。
  void search(const std::string& query, SortKey sort, size_t limit,
              bool dirs_only, bool files_only,
              std::vector<SearchResult>& out, bool& truncated) const;

  // 精确检索：扫描在候选上限处停止（total_capped），否则收集全部匹配（total 精确）。
  // 结果按 sort 排序并裁剪到 limit（limit==0 不限）。under 为绝对路径子树前缀，
  // 空串或 "-" 表示全量；按原始字节比较（大小写敏感），先于名称匹配过滤。
  void searchEx(const std::string& query, SortKey sort, size_t limit,
                bool dirs_only, bool files_only, const std::string& under,
                SearchOutcome& out) const;

  // 仅计数（不物化、不排序）；under 语义同 searchEx。
  size_t countEx(const std::string& query, bool dirs_only, bool files_only,
                 const std::string& under, bool& capped) const;

  // 测试钩子：覆盖候选上限并返回旧值（调用方负责恢复）。
  size_t setCandidateCapForTest(size_t cap) {
    size_t old = candidateCap_;
    candidateCap_ = cap;
    return old;
  }
  size_t candidateCap() const { return candidateCap_; }

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

  // 结果候选上限，避免病态查询拖慢；可通过 setCandidateCapForTest 注入小值。
  size_t candidateCap_ = kDefaultCandidateCap;
};

}  // namespace lsearch
