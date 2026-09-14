#include "core/search.h"

#include "core/db.h"
#include "core/util.h"

#include <algorithm>
#include <cstring>
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

// 匹配计划：内存路径与 sqlite 路径共用同一编译/匹配逻辑，保证逐字节一致。
enum class MatchKind { Substring, Glob, Regex };

struct QueryPlan {
  bool ok = false;             // false = 空查询/空或非法正则 → 空结果
  MatchKind kind = MatchKind::Substring;
  std::string q;               // 子串/glob 用小写查询
  std::regex re;               // regex 用原始模式（编译时 icase）
};

QueryPlan makePlan(const std::string& query) {
  QueryPlan p;
  if (query.empty()) return p;
  if (startsWith(query, "re:")) {
    const std::string pattern = query.substr(3);
    if (trim(pattern).empty()) return p;
    std::string cerr;
    if (!compileRegex(pattern, p.re, cerr)) return p;  // 非法正则 → 空结果（防御）
    p.kind = MatchKind::Regex;
    p.ok = true;
    return p;
  }
  p.q = toLow(query);
  p.kind = (p.q.find('*') != std::string::npos || p.q.find('?') != std::string::npos)
               ? MatchKind::Glob
               : MatchKind::Substring;
  p.ok = true;
  return p;
}

// 与 name_low.find(q) 等价：对 hay 做 ASCII 折叠后按字节查找已小写的 needle。
bool asciiContainsCI(const char* hay, size_t hlen, const std::string& needle) {
  if (needle.empty()) return true;
  if (needle.size() > hlen) return false;
  const size_t last = hlen - needle.size();
  for (size_t i = 0; i <= last; ++i) {
    size_t j = 0;
    for (; j < needle.size(); ++j) {
      char c = hay[i + j];
      if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
      if (c != needle[j]) break;
    }
    if (j == needle.size()) return true;
  }
  return false;
}

bool planMatch(const QueryPlan& p, const char* name, size_t nlen) {
  switch (p.kind) {
    case MatchKind::Substring:
      return asciiContainsCI(name, nlen, p.q);
    case MatchKind::Glob:
      return globMatchView(p.q, name, nlen);
    case MatchKind::Regex:
      try {
        return std::regex_search(name, name + nlen, p.re);
      } catch (const std::regex_error&) {
        return false;
      }
  }
  return false;
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

// 裸字节版 underMatches：path 为 arena/DB 行指针，u 为规范化前缀。
bool underMatchesRaw(const char* path, size_t plen, const std::string& u) {
  if (u.empty()) return true;
  if (plen == u.size()) return std::memcmp(path, u.data(), plen) == 0;
  return plen > u.size() && std::memcmp(path, u.data(), u.size()) == 0 && path[u.size()] == '/';
}

uint64_t hashBytes(const char* p, size_t n) {
  uint64_t h = 1469598103934665603ull;  // FNV-1a
  for (size_t i = 0; i < n; ++i) {
    h ^= static_cast<unsigned char>(p[i]);
    h *= 1099511628211ull;
  }
  return h;
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

// ---------------- Index（arena + 开放寻址哈希） ----------------

bool Index::nameIsPathSuffix(const Entry& e) {
  return e.name_len <= e.path_len && e.name_off == e.path_off + (e.path_len - e.name_len);
}

size_t Index::entryBytes(const Entry& e) {
  return e.path_len + (nameIsPathSuffix(e) ? 0 : e.name_len);
}

void Index::clear() {
  entries_.clear();
  arena_.clear();
  slots_.clear();
  arena_live_ = 0;
  slotUsed_ = 0;
  slotTomb_ = 0;
  dirs_ = 0;
  bytes_ = 0;
}

Index::Entry Index::makeEntry(const FileEntry& fe) {
  Entry en;
  en.size = sanitizeSize(fe.size);
  en.mtime = fe.mtime;
  en.inode = fe.inode;
  en.is_dir = fe.is_dir;
  en.path_off = static_cast<uint32_t>(arena_.size());
  en.path_len = static_cast<uint32_t>(fe.path.size());
  arena_.insert(arena_.end(), fe.path.begin(), fe.path.end());
  if (baseName(fe.path) == fe.name) {
    // 常见情形：name 就是 basename，直接作为 path 的后缀，不重复存储。
    en.name_len = static_cast<uint32_t>(fe.name.size());
    en.name_off = en.path_off + (en.path_len - en.name_len);
  } else {
    en.name_off = static_cast<uint32_t>(arena_.size());
    en.name_len = static_cast<uint32_t>(fe.name.size());
    arena_.insert(arena_.end(), fe.name.begin(), fe.name.end());
  }
  arena_live_ += entryBytes(en);
  return en;
}

SearchResult Index::makeResult(const Entry& e) const {
  SearchResult r;
  r.entry.path.assign(arena_.data() + e.path_off, e.path_len);
  r.entry.name.assign(arena_.data() + e.name_off, e.name_len);
  r.entry.size = e.size;
  r.entry.mtime = e.mtime;
  r.entry.inode = e.inode;
  r.entry.is_dir = e.is_dir;
  r.path_matched = false;
  return r;
}

void Index::build(std::vector<FileEntry>&& in) {
  clear();
  entries_.reserve(in.size());
  // arena 预分配：按 path 字节总量估计，避免增长期多次重分配/拷贝。
  size_t bytesHint = 0;
  for (const auto& fe : in) bytesHint += fe.path.size();
  arena_.reserve(bytesHint);
  for (auto& fe : in) {
    Entry en = makeEntry(fe);
    if (en.is_dir) ++dirs_;
    bytes_.store(satAddBytes(bytes_.load(), static_cast<uint64_t>(en.size)));
    entries_.push_back(en);
  }
  rehashTo(0);  // 依据 entries_ 规模选择容量并插入全部
}

void Index::add(const FileEntry& e) {
  size_t idx = 0;
  size_t slot = tableFindSlot(e.path.data(), e.path.size());
  if (slot != static_cast<size_t>(-1)) {
    idx = slots_[slot] - 1;
    Entry& old = entries_[idx];
    if (old.is_dir) --dirs_;
    bytes_.store(satSubBytes(bytes_.load(), static_cast<uint64_t>(old.size)));
    arena_live_ -= entryBytes(old);
    old = makeEntry(e);
    if (old.is_dir) ++dirs_;
    bytes_.store(satAddBytes(bytes_.load(), static_cast<uint64_t>(old.size)));
    return;
  }
  if (slots_.empty() || (slotUsed_ + slotTomb_ + 1) * 4 > slots_.size() * 3)
    reserveTable(entries_.size() + 1);
  entries_.push_back(makeEntry(e));
  if (entries_.back().is_dir) ++dirs_;
  bytes_.store(satAddBytes(bytes_.load(), static_cast<uint64_t>(entries_.back().size)));
  insertSlot(entries_.size() - 1);
}

bool Index::remove(const std::string& path) {
  size_t slot = tableFindSlot(path.data(), path.size());
  if (slot == static_cast<size_t>(-1)) return false;
  const size_t idx = slots_[slot] - 1;

  const Entry& e = entries_[idx];
  if (e.is_dir) --dirs_;
  bytes_.store(satSubBytes(bytes_.load(), static_cast<uint64_t>(e.size)));
  arena_live_ -= entryBytes(e);

  // remove 的槽位必须先标记墓碑，再覆盖 entries_[idx]（否则按内容比较会失配）。
  slots_[slot] = kHashTomb;
  --slotUsed_;
  ++slotTomb_;

  if (idx + 1 < entries_.size()) {
    const Entry& back = entries_.back();
    const size_t movedSlot = tableFindSlot(arena_.data() + back.path_off, back.path_len);
    entries_[idx] = back;
    if (movedSlot != static_cast<size_t>(-1)) slots_[movedSlot] = static_cast<uint32_t>(idx + 1);
  }
  entries_.pop_back();
  maybeCompactArena();
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
    const char* p = arena_.data() + en.path_off;
    const bool hit =
        (en.path_len == path.size() && std::memcmp(p, path.data(), en.path_len) == 0) ||
        (en.path_len > prefix.size() && std::memcmp(p, prefix.data(), prefix.size()) == 0);
    if (hit) {
      mark[i] = true;
      if (en.is_dir) --dirs_;
      bytes_.store(satSubBytes(bytes_.load(), static_cast<uint64_t>(en.size)));
      arena_live_ -= entryBytes(en);
      ++removed;
    }
  }
  if (removed == 0) return 0;
  std::vector<Entry> keep;
  keep.reserve(entries_.size() - removed);
  for (size_t i = 0; i < entries_.size(); ++i)
    if (!mark[i]) keep.push_back(entries_[i]);
  entries_ = std::move(keep);
  maybeCompactArena();
  rehashTo(0);
  return removed;
}

void Index::collectDirs(std::vector<std::string>& out) const {
  out.clear();
  for (const auto& en : entries_)
    if (en.is_dir) out.emplace_back(arena_.data() + en.path_off, en.path_len);
}

size_t Index::memoryEstimate() const {
  return arena_.capacity() + entries_.capacity() * sizeof(Entry) +
         slots_.capacity() * sizeof(uint32_t);
}

size_t Index::tableFindSlot(const char* path, size_t len) const {
  if (slots_.empty()) return static_cast<size_t>(-1);
  const size_t mask = slots_.size() - 1;
  size_t i = hashBytes(path, len) & mask;
  while (true) {
    const uint32_t v = slots_[i];
    if (v == 0) return static_cast<size_t>(-1);
    if (v != kHashTomb) {
      const Entry& e = entries_[v - 1];
      if (e.path_len == len && std::memcmp(arena_.data() + e.path_off, path, len) == 0)
        return i;
    }
    i = (i + 1) & mask;
  }
}

void Index::rehashTo(size_t cap) {
  if (cap == 0) {
    cap = 16;
    const size_t need = entries_.size();
    while (cap * 3 < need * 4) cap <<= 1;
  }
  slots_.assign(cap, 0);
  slotUsed_ = 0;
  slotTomb_ = 0;
  const size_t mask = cap - 1;
  for (size_t idx = 0; idx < entries_.size(); ++idx) {
    const Entry& e = entries_[idx];
    size_t i = hashBytes(arena_.data() + e.path_off, e.path_len) & mask;
    while (slots_[i] != 0) i = (i + 1) & mask;
    slots_[i] = static_cast<uint32_t>(idx + 1);
    ++slotUsed_;
  }
}

void Index::reserveTable(size_t expected) {
  size_t cap = slots_.empty() ? 16 : slots_.size();
  while (cap * 3 < expected * 4) cap <<= 1;
  rehashTo(cap);
}

void Index::insertSlot(size_t idx) {
  const Entry& e = entries_[idx];
  const size_t mask = slots_.size() - 1;
  size_t i = hashBytes(arena_.data() + e.path_off, e.path_len) & mask;
  size_t firstTomb = static_cast<size_t>(-1);
  while (true) {
    const uint32_t v = slots_[i];
    if (v == 0) {
      if (firstTomb != static_cast<size_t>(-1)) {
        slots_[firstTomb] = static_cast<uint32_t>(idx + 1);
        --slotTomb_;
      } else {
        slots_[i] = static_cast<uint32_t>(idx + 1);
      }
      ++slotUsed_;
      return;
    }
    if (v == kHashTomb && firstTomb == static_cast<size_t>(-1)) firstTomb = i;
    i = (i + 1) & mask;
  }
}

void Index::compactArena() {
  std::vector<char> na;
  na.reserve(arena_live_);
  for (auto& e : entries_) {
    const bool separate = !nameIsPathSuffix(e);
    const uint32_t np = static_cast<uint32_t>(na.size());
    na.insert(na.end(), arena_.data() + e.path_off, arena_.data() + e.path_off + e.path_len);
    if (separate) {
      e.name_off = static_cast<uint32_t>(na.size());
      na.insert(na.end(), arena_.data() + e.name_off, arena_.data() + e.name_off + e.name_len);
    } else {
      e.name_off = np + (e.path_len - e.name_len);
    }
    e.path_off = np;
  }
  arena_.swap(na);
  arena_live_ = 0;
  for (const auto& e : entries_) arena_live_ += entryBytes(e);
}

void Index::maybeCompactArena() {
  const size_t dead = arena_.size() > arena_live_ ? arena_.size() - arena_live_ : 0;
  if (dead > (4u << 20) && dead > arena_live_) compactArena();
}

void Index::search(const std::string& query, SortKey sort, size_t limit,
                   bool dirs_only, bool files_only,
                   std::vector<SearchResult>& out, bool& truncated) const {
  out.clear();
  truncated = false;
  if (query.empty() || entries_.empty()) return;

  const QueryPlan plan = makePlan(query);
  if (!plan.ok) return;

  const size_t cap = candidateCap_;
  if (limit == 0) limit = cap;

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
      if (limit < cap && c >= limit) break;
      const Entry& en = entries_[i];
      if ((dirs_only && !en.is_dir) || (files_only && en.is_dir)) continue;
      if (!planMatch(plan, arena_.data() + en.name_off, en.name_len)) continue;
      count.fetch_add(1, std::memory_order_relaxed);
      local.push_back(makeResult(en));
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

  const QueryPlan plan = makePlan(query);
  if (!plan.ok) return;
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
      if (!underMatchesRaw(arena_.data() + en.path_off, en.path_len, u)) continue;
      if ((dirs_only && !en.is_dir) || (files_only && en.is_dir)) continue;
      if (!planMatch(plan, arena_.data() + en.name_off, en.name_len)) continue;
      // 仅当本线程分配到的槽位 < cap 时收集，保证存储总数恰为 cap（超采样不入列）。
      uint64_t before = count.fetch_add(1, std::memory_order_relaxed);
      if (before >= cap) {
        capped.store(true, std::memory_order_relaxed);
        break;
      }
      local.push_back(makeResult(en));
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

  const QueryPlan plan = makePlan(query);
  if (!plan.ok) return 0;
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
      if (!underMatchesRaw(arena_.data() + en.path_off, en.path_len, u)) continue;
      if ((dirs_only && !en.is_dir) || (files_only && en.is_dir)) continue;
      if (!planMatch(plan, arena_.data() + en.name_off, en.name_len)) continue;
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

// ---------------- sqlite 热路径（Spec 017） ----------------

namespace {

// 流式扫描 files 表并复用与内存路径相同的匹配/过滤/上限逻辑。
// 子串查询把名称过滤下推到 SQL（instr(lower(name), ?)，与 ASCII 小写子串语义一致），
// 其余匹配（glob/regex/under/类型）在 C++ 侧完成。仅物化命中行（≤ stopAt）。
void dbQuery(Db& db, const QueryPlan& plan, bool dirs_only, bool files_only,
             const std::string& u, size_t stopAt, bool materialize,
             std::vector<SearchResult>& out, size_t& count, bool& capped) {
  out.clear();
  count = 0;
  capped = false;
  if (!plan.ok || stopAt == 0) return;

  std::string sql = "SELECT path,name,size,mtime,is_dir,inode FROM files";
  bool hasWhere = false;
  auto addWhere = [&](const char* cond) {
    sql += hasWhere ? " AND " : " WHERE ";
    sql += cond;
    hasWhere = true;
  };
  if (plan.kind == MatchKind::Substring) addWhere("instr(lower(name), ?) > 0");
  if (dirs_only && !files_only) addWhere("is_dir = 1");
  else if (files_only && !dirs_only) addWhere("is_dir = 0");
  // rowid 顺序与内存条目顺序一致（DB 按插入顺序加载），使 cap 截断子集也可复现。
  sql += " ORDER BY rowid";

  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db.raw(), sql.c_str(), -1, &st, nullptr) != SQLITE_OK) return;
  if (plan.kind == MatchKind::Substring)
    sqlite3_bind_text(st, 1, plan.q.c_str(), -1, SQLITE_TRANSIENT);

  while (sqlite3_step(st) == SQLITE_ROW) {
    const char* p = reinterpret_cast<const char*>(sqlite3_column_text(st, 0));
    const char* nm = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
    const int plen = sqlite3_column_bytes(st, 0);
    const int nlen = sqlite3_column_bytes(st, 1);
    if (p == nullptr || nm == nullptr) continue;
    if (!underMatchesRaw(p, static_cast<size_t>(plen), u)) continue;
    const bool isDir = sqlite3_column_int(st, 4) != 0;
    if ((dirs_only && !isDir) || (files_only && isDir)) continue;
    if (!planMatch(plan, nm, static_cast<size_t>(nlen))) continue;
    if (count >= stopAt) {
      capped = true;
      break;
    }
    if (materialize) {
      SearchResult r;
      r.entry.path.assign(p, static_cast<size_t>(plen));
      r.entry.name.assign(nm, static_cast<size_t>(nlen));
      r.entry.size = sanitizeSize(sqlite3_column_int64(st, 2));
      r.entry.mtime = sqlite3_column_int64(st, 3);
      r.entry.inode = static_cast<uint64_t>(sqlite3_column_int64(st, 5));
      r.entry.is_dir = isDir;
      out.push_back(std::move(r));
    }
    ++count;
  }
  sqlite3_finalize(st);
  // 保守：收集数已达上限即视为截断（与 countEx 的 `count>=cap` 语义一致）。
  if (count >= stopAt) capped = true;
}

}  // namespace

void searchDbEx(Db& db, const std::string& query, SortKey sort, size_t limit,
                bool dirs_only, bool files_only, const std::string& under,
                SearchOutcome& out) {
  out.results.clear();
  out.total = 0;
  out.total_capped = false;
  const QueryPlan plan = makePlan(query);
  if (!plan.ok) return;
  const std::string u = normalizeUnder(under);
  size_t count = 0;
  bool capped = false;
  dbQuery(db, plan, dirs_only, files_only, u, Index::kDefaultCandidateCap, true,
          out.results, count, capped);
  sortResults(out.results, sort);
  out.total = out.results.size();
  out.total_capped = capped;
  if (limit > 0 && out.results.size() > limit) out.results.resize(limit);
}

size_t countDbEx(Db& db, const std::string& query, bool dirs_only, bool files_only,
                 const std::string& under, bool& capped) {
  capped = false;
  const QueryPlan plan = makePlan(query);
  if (!plan.ok) return 0;
  const std::string u = normalizeUnder(under);
  std::vector<SearchResult> discard;
  size_t count = 0;
  bool c = false;
  dbQuery(db, plan, dirs_only, files_only, u, Index::kDefaultCandidateCap, false, discard,
          count, c);
  capped = c;
  return count;
}

void searchDbLegacy(Db& db, const std::string& query, SortKey sort, size_t limit,
                    bool dirs_only, bool files_only, std::vector<SearchResult>& out,
                    bool& truncated) {
  out.clear();
  truncated = false;
  const QueryPlan plan = makePlan(query);
  if (!plan.ok) return;
  const size_t cap = Index::kDefaultCandidateCap;
  const size_t lim = limit == 0 ? cap : limit;
  const size_t stopAt = lim < cap ? lim : cap;
  size_t count = 0;
  bool capped = false;
  dbQuery(db, plan, dirs_only, files_only, std::string(), stopAt, true, out, count, capped);
  truncated = out.size() >= cap;
  sortResults(out, sort);
  if (limit > 0 && out.size() > limit) out.resize(limit);
}

}  // namespace lsearch
