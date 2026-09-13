#pragma once
// MCP 纯逻辑层：参数校验/钳制、over-fetch 分页切片、JSON-RPC 报文构造、代际判定。
// 不进行任何 I/O，便于单测覆盖分页与转义边界。
#include "core/entry.h"
#include "core/search.h"
#include "mcp/json.h"

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace mcp {

// JSON-RPC 错误码
inline constexpr int kParseError = -32700;
inline constexpr int kInvalidRequest = -32600;
inline constexpr int kMethodNotFound = -32601;
inline constexpr int kInvalidParams = -32602;

inline constexpr std::size_t kDefaultLimit = 20;
inline constexpr std::size_t kMaxLimit = 200;
inline constexpr long long kTtlMs = 3600000;
inline constexpr const char* kModernVersion = "2026-07-28";
inline constexpr const char* kLegacyVersion = "2025-11-25";
inline constexpr const char* kServerName = "lsearch-mcp";
inline constexpr const char* kServerVersion = "0.1.0";

enum class Kind { Any, Files, Dirs };

enum class Tool { SearchFiles, IndexStats, Unknown };

Tool toolFromName(const std::string& name);

struct SearchArgs {
  std::string query;
  std::size_t limit = kDefaultLimit;
  std::size_t offset = 0;
  lsearch::SortKey sort = lsearch::SortKey::Name;
  Kind kind = Kind::Any;
  std::string under;  // 可选绝对路径子树前缀；空表示全量
};

// 解析并校验 search_files 的 arguments 对象。未知键/类型错误/缺失或空白 query → false。
bool parseSearchArgs(const Json& arguments, SearchArgs& out, std::string& err);

// limit 钳制到 [1,200]（0 或负数 → 1，>200 → 200）。
std::size_t clampLimit(long long v);
// over-fetch 条数 N = min(offset+limit, core 候选上限)。仅旧 daemon 降级路径使用。
std::size_t overFetch(std::size_t offset, std::size_t limit);

struct Page {
  std::vector<lsearch::SearchResult> items;
  std::size_t offset = 0;
  std::size_t limit = 0;
  std::size_t returned = 0;
  bool has_more = false;
  bool truncated = false;
  // search2 精确语义（旧路径下全为 0/false/空）
  std::size_t total = 0;
  bool total_capped = false;
  bool total_is_lower_bound = false;
  std::string hint;
};

// 对 over-fetch 后的结果做 [offset, offset+limit) 切片；页间不重叠。
Page paginate(const std::vector<lsearch::SearchResult>& fetched, std::size_t offset,
              std::size_t limit);

// refetch 命中 cap 时标记本页截断（仅旧 daemon 降级路径使用）。
void applyCapTruncation(Page& page, std::size_t fetchedCount);

// search2 精确结果 → 切片：has_more = !total_capped && offset+returned < total；
// truncated/total_is_lower_bound 取自 total_capped；capped 时填 actionable hint。
Page paginateOutcome(const lsearch::SearchOutcome& outcome, std::size_t offset,
                     std::size_t limit);

// modern stateless 元数据校验：仅当 params._meta 含 io.modelcontextprotocol/protocolVersion
// 时视为 modern 请求（此时必须同时含 clientCapabilities）；缺失或仅含其它键
//（如 legacy progressToken）一律按 legacy 容忍，避免误拒存量客户端。
struct ModernMeta {
  bool hasMeta = false;
  bool valid = false;
  std::string protocolVersion;
  std::string error;
};
ModernMeta checkModernMeta(const Json& request);

// JSON-RPC 报文构造
Json buildResultResponse(const Json& id, Json result);
Json buildErrorResponse(const Json& id, int code, const std::string& message);
std::string renderLine(const Json& message);

// 结果构造
Json discoverResult();
Json toolsListResult();
Json textContent(const std::string& text);
// tools/call 结果：content:[{type:text,text}], isError；modern 时附 resultType。
Json toolTextResult(const std::string& text, bool isError, bool modern);
Json searchPayload(const Page& page, bool rebuilding);
Json statsPayload(const std::vector<std::pair<std::string, std::string>>& kv);

}  // namespace mcp
