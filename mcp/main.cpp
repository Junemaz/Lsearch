// lsearch-mcp — stdio MCP 前端：把 lsearchd 的文件名检索暴露给 LLM 编码代理。
// stdout 只承载换行分隔的 JSON-RPC；所有日志走 stderr；SIGTERM/SIGINT 干净退出且
// 不触碰守护进程生命周期。
#include "core/config.h"
#include "ipc/client.h"
#include "mcp/json.h"
#include "mcp/protocol.h"

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <string>
#include <unistd.h>
#include <utility>
#include <vector>

using namespace lsearch;
using namespace mcp;

namespace {

volatile sig_atomic_t g_stop = 0;

constexpr std::size_t kMaxLineBytes = 1u << 20;

void onSignal(int) { g_stop = 1; }

bool writeAll(int fd, const std::string& s) {
  std::size_t off = 0;
  while (off < s.size()) {
    ssize_t n = ::write(fd, s.data() + off, s.size() - off);
    if (n < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    off += static_cast<std::size_t>(n);
  }
  return true;
}

void emit(const Json& message) { writeAll(STDOUT_FILENO, renderLine(message)); }

struct Session {
  Config cfg;
  Client client;
  bool handshakeSeen = false;
  bool warned = false;
};

bool ensureClient(Session& s, std::string& err) {
  if (s.client.connected()) return true;
  return Client::connectOrSpawn(s.cfg.sock_path, true, s.client, err);
}

// 复用单个连接；传输失败则断开重连并重试一次。
bool doSearch(Session& s, const SearchArgs& a, std::size_t overFetchLimit,
              std::vector<SearchResult>& out, std::string& err) {
  const bool dirsOnly = a.kind == Kind::Dirs;
  const bool filesOnly = a.kind == Kind::Files;
  for (int attempt = 0; attempt < 2; ++attempt) {
    if (!ensureClient(s, err)) return false;
    std::vector<SearchResult> fetched;
    std::size_t total = 0;
    if (s.client.search(a.query, a.sort, overFetchLimit, dirsOnly, filesOnly, fetched, &total,
                        err)) {
      out = std::move(fetched);
      return true;
    }
    s.client.close();
  }
  return false;
}

// search2：精确 total/total_capped + under。旧 daemon 由 Client 内部降级。
bool doSearchEx(Session& s, const SearchArgs& a, std::size_t fetchLimit,
                SearchOutcome& out, std::string& err) {
  const bool dirsOnly = a.kind == Kind::Dirs;
  const bool filesOnly = a.kind == Kind::Files;
  for (int attempt = 0; attempt < 2; ++attempt) {
    if (!ensureClient(s, err)) return false;
    SearchOutcome oc;
    if (s.client.searchEx(a.query, a.sort, fetchLimit, dirsOnly, filesOnly, a.under, oc, err)) {
      out = std::move(oc);
      return true;
    }
    s.client.close();
  }
  return false;
}

// 旧 daemon 降级路径：legacy search + cap refetch（保留旧的分页/截断语义）。
bool doLegacyPage(Session& s, const SearchArgs& sa, Page& page, std::string& err) {
  const std::size_t cap = lsearch::Index::kDefaultCandidateCap;
  const std::size_t n = overFetch(sa.offset, sa.limit);
  std::vector<SearchResult> fetched;
  if (!doSearch(s, sa, n, fetched, err)) return false;
  if (fetched.size() == n && n < cap) {
    std::vector<SearchResult> full;
    if (!doSearch(s, sa, cap, full, err)) return false;
    fetched = std::move(full);
  }
  page = paginate(fetched, sa.offset, sa.limit);
  applyCapTruncation(page, fetched.size());
  return true;
}

bool doStats(Session& s, std::vector<std::pair<std::string, std::string>>& kv, std::string& err) {
  for (int attempt = 0; attempt < 2; ++attempt) {
    if (!ensureClient(s, err)) return false;
    if (s.client.stats(kv, err)) return true;
    s.client.close();
  }
  return false;
}

std::string actionableError(const std::string& detail) {
  return "lsearchd is unavailable or still building its index: " + detail +
         ". Retry in a few seconds; cold start runs a background full scan. Check progress via "
         "index_stats (rebuilding / scan_files / scan_dirs).";
}

void handleToolCall(Session& s, const Json& req, const Json& id, bool modern) {
  const Json* params = req.find("params");
  if (!params || !params->isObject()) {
    emit(buildErrorResponse(id, kInvalidParams, "params must be an object"));
    return;
  }
  const Json* namep = params->find("name");
  if (!namep || !namep->isString()) {
    emit(buildErrorResponse(id, kInvalidParams, "tool name is required"));
    return;
  }
  const std::string name = namep->asString();
  Tool tool = toolFromName(name);
  if (tool == Tool::Unknown) {
    emit(buildErrorResponse(id, kInvalidParams, "unknown tool: " + name));
    return;
  }

  if (tool == Tool::SearchFiles) {
    const Json* args = params->find("arguments");
    const Json nullArgs = Json::makeNull();
    SearchArgs sa;
    std::string perr;
    if (!parseSearchArgs(args ? *args : nullArgs, sa, perr)) {
      emit(buildErrorResponse(id, kInvalidParams, perr));
      return;
    }
    std::string serr;
    Page page;
    bool pageReady = false;
    if (ensureClient(s, serr) && s.client.supportsV2()) {
      SearchOutcome oc;
      if (!doSearchEx(s, sa, sa.offset + sa.limit, oc, serr)) {
        emit(buildResultResponse(id, toolTextResult(actionableError(serr), true, modern)));
        return;
      }
      if (!s.client.usedLegacySearch()) {
        page = paginateOutcome(oc, sa.offset, sa.limit);
        pageReady = true;
      }
    }
    if (!pageReady && !doLegacyPage(s, sa, page, serr)) {
      emit(buildResultResponse(id, toolTextResult(actionableError(serr), true, modern)));
      return;
    }
    bool rebuilding = false;
    std::vector<std::pair<std::string, std::string>> kv;
    std::string sterr;
    if (doStats(s, kv, sterr)) {
      for (const auto& e : kv)
        if (e.first == "rebuilding") rebuilding = (e.second == "1");
    }
    emit(buildResultResponse(id, toolTextResult(searchPayload(page, rebuilding).dump(), false, modern)));
    return;
  }

  const Json* args = params->find("arguments");
  if (args && !(args->isObject() && args->size() == 0)) {
    emit(buildErrorResponse(id, kInvalidParams, "index_stats takes no arguments"));
    return;
  }
  std::vector<std::pair<std::string, std::string>> kv;
  std::string sterr;
  if (!doStats(s, kv, sterr)) {
    emit(buildResultResponse(id, toolTextResult(actionableError(sterr), true, modern)));
    return;
  }
  emit(buildResultResponse(id, toolTextResult(statsPayload(kv).dump(), false, modern)));
}

void handleLine(Session& s, const std::string& line) {
  Json req;
  std::string perr;
  if (!Json::parse(line, req, perr)) {
    emit(buildErrorResponse(Json::makeNull(), kParseError, "parse error: " + perr));
    return;
  }
  if (!req.isObject()) {
    emit(buildErrorResponse(Json::makeNull(), kInvalidRequest, "request must be an object"));
    return;
  }
  const Json* mp = req.find("method");
  const Json* idp = req.find("id");
  const bool hasId = idp != nullptr && !idp->isNull();
  const Json id = hasId ? *idp : Json::makeNull();
  if (!mp || !mp->isString() || mp->asString().empty()) {
    emit(buildErrorResponse(id, kInvalidRequest, "method is required"));
    return;
  }
  const std::string method = mp->asString();
  const bool isNotification = !hasId || method.rfind("notifications/", 0) == 0;

  ModernMeta meta = checkModernMeta(req);
  if (meta.hasMeta && !meta.valid) {
    if (hasId) emit(buildErrorResponse(id, kInvalidParams, meta.error));
    return;
  }
  if (meta.valid) s.handshakeSeen = true;

  if (method == "initialize") {
    s.handshakeSeen = true;
    std::string ver = kLegacyVersion;
    const Json* params = req.find("params");
    if (params && params->isObject()) {
      const Json* cv = params->find("protocolVersion");
      if (cv && cv->isString()) {
        const std::string& v = cv->asString();
        if (v == kModernVersion || v == kLegacyVersion) ver = v;
      }
    }
    Json result = Json::object();
    result.set("protocolVersion", Json::str(ver));
    Json caps = Json::object();
    caps.set("tools", Json::object());
    result.set("capabilities", std::move(caps));
    Json si = Json::object();
    si.set("name", Json::str(kServerName));
    si.set("version", Json::str(kServerVersion));
    result.set("serverInfo", std::move(si));
    if (hasId) emit(buildResultResponse(id, std::move(result)));
    return;
  }

  if (isNotification) return;

  if (method == "ping") {
    emit(buildResultResponse(id, Json::object()));
    return;
  }

  if (!meta.hasMeta && !s.handshakeSeen && !s.warned) {
    s.warned = true;
    fprintf(stderr,
            "[lsearch-mcp] warning: request without initialize and without params._meta; "
            "handling as legacy\n");
  }

  if (method == "server/discover") {
    emit(buildResultResponse(id, discoverResult()));
    return;
  }
  if (method == "tools/list") {
    emit(buildResultResponse(id, toolsListResult()));
    return;
  }
  if (method == "tools/call") {
    handleToolCall(s, req, id, meta.valid);
    return;
  }
  fprintf(stderr, "[lsearch-mcp] unknown method: %s\n", method.c_str());
  if (hasId) emit(buildErrorResponse(id, kMethodNotFound, "method not found: " + method));
}

}  // namespace

int main(int, char**) {
  signal(SIGPIPE, SIG_IGN);
  struct sigaction sa;
  std::memset(&sa, 0, sizeof(sa));
  sa.sa_handler = onSignal;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;  // 令阻塞中的 read 被信号打断返回 EINTR
  sigaction(SIGTERM, &sa, nullptr);
  sigaction(SIGINT, &sa, nullptr);

  Session s;
  s.cfg = Config::load("");

  std::string buf;
  bool discarding = false;
  char tmp[8192];
  while (!g_stop) {
    std::size_t nl;
    while ((nl = buf.find('\n')) != std::string::npos) {
      std::string line = buf.substr(0, nl);
      buf.erase(0, nl + 1);
      if (discarding) {
        discarding = false;
        continue;
      }
      if (line.size() > kMaxLineBytes) {
        fprintf(stderr, "[lsearch-mcp] input line too large (%zu bytes); dropped\n", line.size());
        emit(buildErrorResponse(Json::makeNull(), kParseError, "request line too large"));
        continue;
      }
      if (!line.empty() && line.back() == '\r') line.pop_back();
      if (line.empty()) continue;
      handleLine(s, line);
    }
    if (buf.size() > kMaxLineBytes) {
      buf.clear();
      discarding = true;
      fprintf(stderr, "[lsearch-mcp] input exceeded %zu bytes; dropping until newline\n",
              kMaxLineBytes);
      emit(buildErrorResponse(Json::makeNull(), kParseError, "request line too large"));
      continue;
    }
    ssize_t n = ::read(STDIN_FILENO, tmp, sizeof(tmp));
    if (n > 0) {
      buf.append(tmp, static_cast<std::size_t>(n));
      continue;
    }
    if (n == 0) break;  // stdin EOF
    if (errno == EINTR) {
      if (g_stop) break;
      continue;
    }
    break;
  }
  return 0;
}
