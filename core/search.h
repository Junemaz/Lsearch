#pragma once
// 内存热索引：守护进程常驻，支持"即输即搜"
// 语义：仅按**最终文件/文件夹名（basename）**做大小写不敏感的子串匹配；
// 查询以 `re:` 开头时切换为 ECMAScript 正则（大小写不敏感，regex_search），
// 否则含 * 或 ? 时切换为 glob 匹配。提供按名称/路径/大小/修改时间排序。
#include "core/entry.h"

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace lsearch {

class Db;

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

  // 常驻内存估算（Spec 017）：arena + 条目数组 + 开放寻址哈希表的容量。
  size_t memoryEstimate() const;

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
  // 紧凑条目：path/name 字节统一存入 arena_，条目只保留偏移（Spec 017）。
  // name 若恰为 path 的 basename 后缀则不重复存储，name_off 为 path 内偏移。
  struct Entry {
    uint32_t path_off = 0, path_len = 0;
    uint32_t name_off = 0, name_len = 0;
    int64_t size = 0, mtime = 0;
    uint64_t inode = 0;
    bool is_dir = false;
  };
  // 开放寻址槽位：0=空，kHashTomb=墓碑，其余为 entries_ 下标+1。
  static constexpr uint32_t kHashTomb = 0xFFFFFFFFu;

  Entry makeEntry(const FileEntry& fe);
  SearchResult makeResult(const Entry& e) const;
  static size_t entryBytes(const Entry& e);
  static bool nameIsPathSuffix(const Entry& e);

  size_t tableFindSlot(const char* path, size_t len) const;
  void rehashTo(size_t cap);
  void reserveTable(size_t expected);
  void insertSlot(size_t idx);

  void compactArena();
  void maybeCompactArena();

  std::vector<char> arena_;
  size_t arena_live_ = 0;  // 存活条目实际占用字节（用于判断 arena 碎片）
  std::vector<Entry> entries_;
  std::vector<uint32_t> slots_;
  size_t slotUsed_ = 0, slotTomb_ = 0;

  std::atomic<uint64_t> dirs_{0};
  std::atomic<uint64_t> bytes_{0};

  // 结果候选上限，避免病态查询拖慢；可通过 setCandidateCapForTest 注入小值。
  size_t candidateCap_ = kDefaultCandidateCap;
};

// hot_index=sqlite（Spec 017）：不常驻全量条目，查询直接走 SQLite。
// 语义与 Index 内存路径逐字节一致（同一匹配/排序/裁剪逻辑）。
// 并发契约（Spec 015）：调用方必须持有守护进程的 dbM_ 独占锁。
void searchDbEx(Db& db, const std::string& query, SortKey sort, size_t limit,
                bool dirs_only, bool files_only, const std::string& under,
                SearchOutcome& out);
void searchDbLegacy(Db& db, const std::string& query, SortKey sort, size_t limit,
                    bool dirs_only, bool files_only, std::vector<SearchResult>& out,
                    bool& truncated);
size_t countDbEx(Db& db, const std::string& query, bool dirs_only, bool files_only,
                 const std::string& under, bool& capped);

}  // namespace lsearch
