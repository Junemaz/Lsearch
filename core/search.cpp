#include "core/search.h"

#include "core/util.h"

#include <algorithm>
#include <thread>

namespace lsearch {

static std::string toLow(const std::string& s) { return lsearch::toLowerAscii(s); }

void Index::clear() {
  entries_.clear();
  pathIndex_.clear();
  dirs_ = 0;
  bytes_ = 0;
}

void Index::build(std::vector<FileEntry>&& in) {
  clear();
  entries_.reserve(in.size());
  for (auto& fe : in) {
    Entry en;
    en.e = std::move(fe);
    en.name_low = toLow(en.e.name);
    en.path_low = toLow(en.e.path);
    if (en.e.is_dir) ++dirs_;
    bytes_ += static_cast<uint64_t>(en.e.size);
    pathIndex_.emplace(en.e.path, entries_.size());
    entries_.push_back(std::move(en));
  }
}

void Index::add(const FileEntry& e) {
  Entry en;
  en.e = e;
  en.name_low = toLow(e.name);
  en.path_low = toLow(e.path);
  auto it = pathIndex_.find(e.path);
  if (it != pathIndex_.end()) {
    // 已存在：原地更新
    Entry& old = entries_[it->second];
    if (old.e.is_dir) --dirs_;
    bytes_ -= static_cast<uint64_t>(old.e.size);
    old = std::move(en);
  } else {
    if (e.is_dir) ++dirs_;
    bytes_ += static_cast<uint64_t>(e.size);
    pathIndex_.emplace(e.path, entries_.size());
    entries_.push_back(std::move(en));
  }
}

bool Index::remove(const std::string& path) {
  auto it = pathIndex_.find(path);
  if (it == pathIndex_.end()) return false;
  size_t idx = it->second;
  const Entry& e = entries_[idx];
  if (e.e.is_dir) --dirs_;
  if (e.e.size > 0) bytes_ -= static_cast<uint64_t>(e.e.size);
  // 用末尾元素覆盖，保持 O(1)
  if (idx + 1 < entries_.size()) {
    const std::string movedPath = entries_.back().e.path;
    entries_[idx] = std::move(entries_.back());
    pathIndex_[movedPath] = idx;
  }
  entries_.pop_back();
  pathIndex_.erase(path);
  return true;
}

void Index::replace(const std::string& oldPath, const FileEntry& e) {
  if (oldPath == e.path) {
    add(e);
    return;
  }
  remove(oldPath);
  add(e);
}

size_t Index::removePathAndSubtree(const std::string& path) {
  const std::string prefix = path + "/";
  std::vector<bool> mark(entries_.size(), false);
  size_t removed = 0;
  for (size_t i = 0; i < entries_.size(); ++i) {
    const Entry& en = entries_[i];
    if (en.e.path == path || startsWith(en.e.path, prefix)) {
      mark[i] = true;
      if (en.e.is_dir) --dirs_;
      bytes_ -= static_cast<uint64_t>(en.e.size);
      ++removed;
    }
  }
  if (removed == 0) return 0;
  std::vector<Entry> keep;
  keep.reserve(entries_.size() - removed);
  pathIndex_.clear();
  for (size_t i = 0; i < entries_.size(); ++i) {
    if (mark[i]) continue;
    pathIndex_.emplace(entries_[i].e.path, keep.size());
    keep.push_back(std::move(entries_[i]));
  }
  entries_ = std::move(keep);
  return removed;
}

void Index::collectDirs(std::vector<std::string>& out) const {
  out.clear();
  for (const auto& en : entries_)
    if (en.e.is_dir) out.push_back(en.e.path);
}

void Index::search(const std::string& query, SortKey sort, size_t limit,
                   bool dirs_only, bool files_only,
                   std::vector<SearchResult>& out, bool& truncated) const {
  out.clear();
  truncated = false;
  if (query.empty() || entries_.empty()) return;

  const std::string q = toLow(query);
  const bool hasWildcard = q.find('*') != std::string::npos || q.find('?') != std::string::npos;

  if (limit == 0) limit = kCandidateCap;

  // 并行分块扫描（各线程收集自己的候选，最后合并排序裁剪）
  unsigned nthreads = std::thread::hardware_concurrency();
  if (nthreads == 0) nthreads = 2;
  if (nthreads > 32) nthreads = 32;
  const size_t n = entries_.size();
  if (nthreads > n) nthreads = static_cast<unsigned>(n ? n : 1);

  std::vector<std::vector<SearchResult>> perThread(nthreads);
  std::atomic<uint64_t> count{0};

  auto worker = [&](unsigned tid) {
    size_t begin = (n * tid) / nthreads;
    size_t end = (n * (tid + 1)) / nthreads;
    auto& local = perThread[tid];
    local.reserve(256);
    for (size_t i = begin; i < end; ++i) {
      uint64_t c = count.load(std::memory_order_relaxed);
      if (c >= kCandidateCap) break;                       // 兜底：避免病态查询拖垮
      if (limit != 0 && limit < kCandidateCap && c >= limit) break;
      const Entry& en = entries_[i];
      if ((dirs_only && !en.e.is_dir) || (files_only && en.e.is_dir)) continue;
      // 语义：仅按最终文件/文件夹名（basename）匹配，不匹配完整路径
      bool hit;
      if (hasWildcard)
        hit = globMatch(q, en.name_low);
      else
        hit = en.name_low.find(q) != std::string::npos;
      if (!hit) continue;
      count.fetch_add(1, std::memory_order_relaxed);
      local.push_back({en.e, false});
    }
  };

  // 等待计数达到 limit 时提前终止所有线程
  // （简单实现：全部跑完；'a' 这类病态查询由 kCandidateCap 兜底，够 Home 规模用）
  std::vector<std::thread> threads;
  for (unsigned t = 0; t < nthreads; ++t) threads.emplace_back(worker, t);
  for (auto& th : threads) th.join();

  size_t total = 0;
  for (auto& v : perThread) total += v.size();
  truncated = total >= kCandidateCap;

  out.reserve(total);
  for (auto& v : perThread)
    for (auto& r : v) out.push_back(std::move(r));

  // 排序
  switch (sort) {
    case SortKey::Name:
      std::sort(out.begin(), out.end(), [](const SearchResult& a, const SearchResult& b) {
        int c = a.entry.name.compare(b.entry.name);
        if (c != 0) return c < 0;
        return a.entry.path < b.entry.path;
      });
      break;
    case SortKey::Path:
      std::sort(out.begin(), out.end(), [](const SearchResult& a, const SearchResult& b) {
        return a.entry.path < b.entry.path;
      });
      break;
    case SortKey::Size:
      std::sort(out.begin(), out.end(), [](const SearchResult& a, const SearchResult& b) {
        if (a.entry.size != b.entry.size) return a.entry.size > b.entry.size;
        return a.entry.path < b.entry.path;
      });
      break;
    case SortKey::Mtime:
      std::sort(out.begin(), out.end(), [](const SearchResult& a, const SearchResult& b) {
        if (a.entry.mtime != b.entry.mtime) return a.entry.mtime > b.entry.mtime;
        return a.entry.path < b.entry.path;
      });
      break;
  }
  if (limit > 0 && out.size() > limit) out.resize(limit);
}

}  // namespace lsearch
