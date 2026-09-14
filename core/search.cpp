#include "core/search.h"

#include "core/util.h"

#include <algorithm>
#include <regex>
#include <thread>

namespace lsearch {

static std::string toLow(const std::string& s) { return lsearch::toLowerAscii(s); }

namespace {

// 饱和算术：任何统计累加都应"封顶不回绕"，避免单条异常 size 或状态漂移
// 把 totalBytes() 变成 2^63 量级的假值（Spec 016 F3）。
uint64_t satAddBytes(uint64_t acc, uint64_t v) {
  uint64_t s = acc + v;
  return s < acc ? UINT64_MAX : s;
}
uint64_t satSubBytes(uint64_t acc, uint64_t v) { return acc > v ? acc - v : 0; }

// 与 Index::search 共用的正则编译路径：ECMAScript + 大小写不敏感。
// pattern 必须保持原始大小写，小写化会改变 \D/\W/\S 等转义语义。
bool compileRegex(const std::string& pattern, std::regex& out, std::string& err) {
  try {
    out = std::regex(pattern, std::regex::ECMAScript | std::regex::icase);
    return true;
  } catch (const std::regex_error& e) {
    err = e.what();
    return false;
  }
}

void sortResults(std::vector<SearchResult>& out, SortKey sort) {
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
}

// ""/"/"/"-" = 全量；其余去掉尾部 '/'（按原始字节，大小写敏感）。
std::string normalizeUnder(const std::string& in) {
  if (in.empty() || in == "-") return std::string();
  std::string u = in;
  while (u.size() > 1 && u.back() == '/') u.pop_back();
  if (u == "/") return std::string();
  return u;
}

bool underMatches(const std::string& path, const std::string& u) {
  if (u.empty()) return true;
  if (path == u) return true;
  return path.size() > u.size() && startsWith(path, u) && path[u.size()] == '/';
}

}  // namespace

bool validateQuery(const std::string& query, std::string& err) {
  err.clear();
  if (!startsWith(query, "re:")) return true;
  const std::string pattern = query.substr(3);
  if (trim(pattern).empty()) return true;  // re: 空/全空白 → 合法空查询
  std::regex re;
  return compileRegex(pattern, re, err);
}

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
    en.e.size = sanitizeSize(en.e.size);
    en.name_low = toLow(en.e.name);
    en.path_low = toLow(en.e.path);
    if (en.e.is_dir) ++dirs_;
    bytes_.store(satAddBytes(bytes_.load(), static_cast<uint64_t>(en.e.size)));
    pathIndex_.emplace(en.e.path, entries_.size());
    entries_.push_back(std::move(en));
  }
}

void Index::add(const FileEntry& e) {
  Entry en;
  en.e = e;
  en.e.size = sanitizeSize(en.e.size);
  en.name_low = toLow(e.name);
  en.path_low = toLow(e.path);
  auto it = pathIndex_.find(e.path);
  if (it != pathIndex_.end()) {
    // 已存在：原地更新。注意必须先减旧值、再加新值，漏加会导致 bytes_ 单调下溢。
    Entry& old = entries_[it->second];
    if (old.e.is_dir) --dirs_;
    bytes_.store(satSubBytes(bytes_.load(), static_cast<uint64_t>(old.e.size)));
    old = std::move(en);
    bytes_.store(satAddBytes(bytes_.load(), static_cast<uint64_t>(old.e.size)));
  } else {
    if (e.is_dir) ++dirs_;
    bytes_.store(satAddBytes(bytes_.load(), static_cast<uint64_t>(en.e.size)));
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
  bytes_.store(satSubBytes(bytes_.load(), static_cast<uint64_t>(e.e.size)));
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
      bytes_.store(satSubBytes(bytes_.load(), static_cast<uint64_t>(en.e.size)));
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

  const bool isRegex = startsWith(query, "re:");
  std::regex re;
  if (isRegex) {
    const std::string pattern = query.substr(3);
    if (trim(pattern).empty()) return;  // re: 空/全空白 → 空结果
    std::string cerr;
    if (!compileRegex(pattern, re, cerr)) return;  // 防御：非法正则视为空结果
  }

  const std::string q = toLow(query);
  const bool hasWildcard =
      !isRegex && (q.find('*') != std::string::npos || q.find('?') != std::string::npos);

  const size_t cap = candidateCap_;
  if (limit == 0) limit = cap;

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
      if (c >= cap) break;                       // 兜底：避免病态查询拖垮
      if (limit != 0 && limit < cap && c >= limit) break;
      const Entry& en = entries_[i];
      if ((dirs_only && !en.e.is_dir) || (files_only && en.e.is_dir)) continue;
      // 语义：仅按最终文件/文件夹名（basename）匹配，不匹配完整路径
      bool hit;
      if (isRegex) {
        try {
          hit = std::regex_search(en.e.name, re);  // 原始 basename + icase，禁止对小写名匹配
        } catch (const std::regex_error&) {
          hit = false;  // 匹配期异常不得逸出 worker（否则 std::terminate 崩溃守护进程）
        }
      } else if (hasWildcard) {
        hit = globMatch(q, en.name_low);
      } else {
        hit = en.name_low.find(q) != std::string::npos;
      }
      if (!hit) continue;
      count.fetch_add(1, std::memory_order_relaxed);
      local.push_back({en.e, false});
    }
  };

  // 等待计数达到 limit 时提前终止所有线程
  // （简单实现：全部跑完；'a' 这类病态查询由候选上限 candidateCap_ 兜底，够 Home 规模用）
  std::vector<std::thread> threads;
  for (unsigned t = 0; t < nthreads; ++t) threads.emplace_back(worker, t);
  for (auto& th : threads) th.join();

  size_t total = 0;
  for (auto& v : perThread) total += v.size();
  truncated = total >= cap;

  out.reserve(total);
  for (auto& v : perThread)
    for (auto& r : v) out.push_back(std::move(r));

  sortResults(out, sort);
  if (limit > 0 && out.size() > limit) out.resize(limit);
}

void Index::searchEx(const std::string& query, SortKey sort, size_t limit,
                     bool dirs_only, bool files_only, const std::string& under,
                     SearchOutcome& out) const {
  out.results.clear();
  out.total = 0;
  out.total_capped = false;
  if (query.empty() || entries_.empty()) return;

  const bool isRegex = startsWith(query, "re:");
  std::regex re;
  if (isRegex) {
    const std::string pattern = query.substr(3);
    if (trim(pattern).empty()) return;
    std::string cerr;
    if (!compileRegex(pattern, re, cerr)) return;
  }
  const std::string q = toLow(query);
  const bool hasWildcard =
      !isRegex && (q.find('*') != std::string::npos || q.find('?') != std::string::npos);
  const std::string u = normalizeUnder(under);
  const size_t cap = candidateCap_;

  unsigned nthreads = std::thread::hardware_concurrency();
  if (nthreads == 0) nthreads = 2;
  if (nthreads > 32) nthreads = 32;
  const size_t n = entries_.size();
  if (nthreads > n) nthreads = static_cast<unsigned>(n ? n : 1);

  std::vector<std::vector<SearchResult>> perThread(nthreads);
  std::atomic<uint64_t> count{0};
  std::atomic<bool> capped{false};

  auto worker = [&](unsigned tid) {
    size_t begin = (n * tid) / nthreads;
    size_t end = (n * (tid + 1)) / nthreads;
    auto& local = perThread[tid];
    local.reserve(256);
    for (size_t i = begin; i < end; ++i) {
      if (count.load(std::memory_order_relaxed) >= cap) {
        capped.store(true, std::memory_order_relaxed);
        break;
      }
      const Entry& en = entries_[i];
      if (!underMatches(en.e.path, u)) continue;
      if ((dirs_only && !en.e.is_dir) || (files_only && en.e.is_dir)) continue;
      bool hit;
      if (isRegex) {
        try {
          hit = std::regex_search(en.e.name, re);
        } catch (const std::regex_error&) {
          hit = false;
        }
      } else if (hasWildcard) {
        hit = globMatch(q, en.name_low);
      } else {
        hit = en.name_low.find(q) != std::string::npos;
      }
      if (!hit) continue;
      // 仅当本线程分配到的槽位 < cap 时收集，保证存储总数恰为 cap（超采样不入列）。
      uint64_t before = count.fetch_add(1, std::memory_order_relaxed);
      if (before >= cap) {
        capped.store(true, std::memory_order_relaxed);
        break;
      }
      local.push_back({en.e, false});
    }
  };

  std::vector<std::thread> threads;
  for (unsigned t = 0; t < nthreads; ++t) threads.emplace_back(worker, t);
  for (auto& th : threads) th.join();

  size_t total = 0;
  for (auto& v : perThread) total += v.size();
  out.total_capped = capped.load(std::memory_order_relaxed);
  out.total = total;

  out.results.reserve(total);
  for (auto& v : perThread)
    for (auto& r : v) out.results.push_back(std::move(r));

  sortResults(out.results, sort);
  if (limit > 0 && out.results.size() > limit) out.results.resize(limit);
}

size_t Index::countEx(const std::string& query, bool dirs_only, bool files_only,
                      const std::string& under, bool& capped) const {
  capped = false;
  if (query.empty() || entries_.empty()) return 0;

  const bool isRegex = startsWith(query, "re:");
  std::regex re;
  if (isRegex) {
    const std::string pattern = query.substr(3);
    if (trim(pattern).empty()) return 0;
    std::string cerr;
    if (!compileRegex(pattern, re, cerr)) return 0;
  }
  const std::string q = toLow(query);
  const bool hasWildcard =
      !isRegex && (q.find('*') != std::string::npos || q.find('?') != std::string::npos);
  const std::string u = normalizeUnder(under);
  const size_t cap = candidateCap_;

  unsigned nthreads = std::thread::hardware_concurrency();
  if (nthreads == 0) nthreads = 2;
  if (nthreads > 32) nthreads = 32;
  const size_t n = entries_.size();
  if (nthreads > n) nthreads = static_cast<unsigned>(n ? n : 1);

  std::atomic<uint64_t> count{0};
  std::atomic<bool> cappedFlag{false};

  auto worker = [&](unsigned tid) {
    size_t begin = (n * tid) / nthreads;
    size_t end = (n * (tid + 1)) / nthreads;
    for (size_t i = begin; i < end; ++i) {
      if (count.load(std::memory_order_relaxed) >= cap) {
        cappedFlag.store(true, std::memory_order_relaxed);
        break;
      }
      const Entry& en = entries_[i];
      if (!underMatches(en.e.path, u)) continue;
      if ((dirs_only && !en.e.is_dir) || (files_only && en.e.is_dir)) continue;
      bool hit;
      if (isRegex) {
        try {
          hit = std::regex_search(en.e.name, re);
        } catch (const std::regex_error&) {
          hit = false;
        }
      } else if (hasWildcard) {
        hit = globMatch(q, en.name_low);
      } else {
        hit = en.name_low.find(q) != std::string::npos;
      }
      if (!hit) continue;
      uint64_t before = count.fetch_add(1, std::memory_order_relaxed);
      if (before >= cap) {
        cappedFlag.store(true, std::memory_order_relaxed);
        break;
      }
    }
  };

  std::vector<std::thread> threads;
  for (unsigned t = 0; t < nthreads; ++t) threads.emplace_back(worker, t);
  for (auto& th : threads) th.join();

  uint64_t matched = count.load(std::memory_order_relaxed);
  if (matched > cap) matched = cap;
  capped = cappedFlag.load(std::memory_order_relaxed) || count.load() >= cap;
  return static_cast<size_t>(matched);
}

}  // namespace lsearch
