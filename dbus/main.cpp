// lsearch-dbus — 会话 D-Bus 薄桥接：把 lsearchd 暴露为标准桌面服务 com.lsearch.Daemon。
// 仅复用 ipc/client，不触碰 core/daemon/socket 协议；单线程事件循环（poll + libdbus）。
// 生命周期：按需激活，会话总线断开或收到 SIGTERM/SIGINT 时释放名字并退出 0，绝不停止 lsearchd。
#include "core/config.h"
#include "core/util.h"
#include "dbus/logic.h"
#include "ipc/client.h"
#include "mcp/protocol.h"

#include <dbus/dbus.h>

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <poll.h>
#include <string>
#include <time.h>
#include <unistd.h>
#include <utility>
#include <vector>

using namespace lsearch;
using namespace lsearch::dbusbridge;

namespace {

constexpr const char* kBusName = "com.lsearch.Daemon";
constexpr const char* kObjectPath = "/com/lsearch/Daemon";
constexpr const char* kInterface = "com.lsearch.Daemon1";
constexpr const char* kErrInvalidArgs = "com.lsearch.Error.InvalidArgs";
constexpr const char* kErrDaemonUnavailable = "com.lsearch.Error.DaemonUnavailable";
constexpr const char* kErrBusy = "com.lsearch.Error.Busy";

// 桥接自身版本 + 接口标记，便于客户端确认 talks-to。
constexpr const char* kBridgeVersion = "lsearch-dbus 0.1.0 (interface com.lsearch.Daemon1)";

volatile sig_atomic_t g_stop = 0;

void onSignal(int) { g_stop = 1; }

const char* kIntrospectXml =
    "<!DOCTYPE node PUBLIC \"-//freedesktop//DTD D-BUS Object Introspection 1.0//EN\"\n"
    " \"http://www.freedesktop.org/standards/dbus/1.0/introspect.dtd\">\n"
    "<node name=\"/com/lsearch/Daemon\">\n"
    "  <interface name=\"com.lsearch.Daemon1\">\n"
    "    <method name=\"Search\">\n"
    "      <arg name=\"query\" type=\"s\" direction=\"in\"/>\n"
    "      <arg name=\"limit\" type=\"u\" direction=\"in\"/>\n"
    "      <arg name=\"offset\" type=\"u\" direction=\"in\"/>\n"
    "      <arg name=\"sort\" type=\"s\" direction=\"in\"/>\n"
    "      <arg name=\"kind\" type=\"s\" direction=\"in\"/>\n"
    "      <arg name=\"results_json\" type=\"s\" direction=\"out\"/>\n"
    "    </method>\n"
    "    <method name=\"Stats\">\n"
    "      <arg name=\"stats\" type=\"a{sv}\" direction=\"out\"/>\n"
    "    </method>\n"
    "    <method name=\"Rebuild\"/>\n"
    "    <method name=\"Version\">\n"
    "      <arg name=\"version\" type=\"s\" direction=\"out\"/>\n"
    "    </method>\n"
    "  </interface>\n"
    "  <interface name=\"org.freedesktop.DBus.Introspectable\">\n"
    "    <method name=\"Introspect\">\n"
    "      <arg name=\"xml_data\" type=\"s\" direction=\"out\"/>\n"
    "    </method>\n"
    "  </interface>\n"
    "  <interface name=\"org.freedesktop.DBus.Peer\">\n"
    "    <method name=\"Ping\"/>\n"
    "  </interface>\n"
    "</node>\n";

struct Bridge {
  Config cfg;
  Client client;
  RebuildThrottle throttle;
};

void sendError(DBusConnection* conn, DBusMessage* msg, const char* name,
               const std::string& text) {
  DBusMessage* reply = dbus_message_new_error(msg, name, text.c_str());
  if (!reply) return;
  dbus_connection_send(conn, reply, nullptr);
  dbus_message_unref(reply);
}

void replyString(DBusConnection* conn, DBusMessage* msg, const std::string& s) {
  DBusMessage* reply = dbus_message_new_method_return(msg);
  const char* p = s.c_str();
  dbus_message_append_args(reply, DBUS_TYPE_STRING, &p, DBUS_TYPE_INVALID);
  dbus_connection_send(conn, reply, nullptr);
  dbus_message_unref(reply);
}

void replyEmpty(DBusConnection* conn, DBusMessage* msg) {
  DBusMessage* reply = dbus_message_new_method_return(msg);
  dbus_connection_send(conn, reply, nullptr);
  dbus_message_unref(reply);
}

void sendStats(DBusConnection* conn, DBusMessage* msg,
               const std::vector<StatVariant>& stats) {
  DBusMessage* reply = dbus_message_new_method_return(msg);
  DBusMessageIter root, arr;
  dbus_message_iter_init_append(reply, &root);
  dbus_message_iter_open_container(&root, DBUS_TYPE_ARRAY, "{sv}", &arr);
  for (const auto& s : stats) {
    DBusMessageIter de, var;
    dbus_message_iter_open_container(&arr, DBUS_TYPE_DICT_ENTRY, nullptr, &de);
    const char* key = s.key.c_str();
    dbus_message_iter_append_basic(&de, DBUS_TYPE_STRING, &key);
    switch (s.type) {
      case StatVariant::Type::U32: {
        dbus_message_iter_open_container(&de, DBUS_TYPE_VARIANT, "u", &var);
        dbus_uint32_t v = s.u32;
        dbus_message_iter_append_basic(&var, DBUS_TYPE_UINT32, &v);
        dbus_message_iter_close_container(&de, &var);
        break;
      }
      case StatVariant::Type::U64: {
        dbus_message_iter_open_container(&de, DBUS_TYPE_VARIANT, "t", &var);
        dbus_uint64_t v = s.u64;
        dbus_message_iter_append_basic(&var, DBUS_TYPE_UINT64, &v);
        dbus_message_iter_close_container(&de, &var);
        break;
      }
      case StatVariant::Type::Bool: {
        dbus_message_iter_open_container(&de, DBUS_TYPE_VARIANT, "b", &var);
        dbus_bool_t v = s.boolean ? TRUE : FALSE;
        dbus_message_iter_append_basic(&var, DBUS_TYPE_BOOLEAN, &v);
        dbus_message_iter_close_container(&de, &var);
        break;
      }
      case StatVariant::Type::Str: {
        dbus_message_iter_open_container(&de, DBUS_TYPE_VARIANT, "s", &var);
        const char* v = s.str.c_str();
        dbus_message_iter_append_basic(&var, DBUS_TYPE_STRING, &v);
        dbus_message_iter_close_container(&de, &var);
        break;
      }
    }
    dbus_message_iter_close_container(&arr, &de);
  }
  dbus_message_iter_close_container(&root, &arr);
  dbus_connection_send(conn, reply, nullptr);
  dbus_message_unref(reply);
}

bool ensureClient(Bridge& b, std::string& err) {
  if (b.client.connected()) return true;
  return Client::connectOrSpawn(b.cfg.sock_path, true, b.client, err);
}

// 复用单个连接；传输失败则断开重连并重试一次（与 MCP 前端同策略）。
bool doSearch(Bridge& b, const mcp::SearchArgs& a, std::size_t overFetchLimit,
              std::vector<SearchResult>& out, std::string& err) {
  const bool dirsOnly = a.kind == mcp::Kind::Dirs;
  const bool filesOnly = a.kind == mcp::Kind::Files;
  for (int attempt = 0; attempt < 2; ++attempt) {
    if (!ensureClient(b, err)) return false;
    std::vector<SearchResult> fetched;
    std::size_t total = 0;
    if (b.client.search(a.query, a.sort, overFetchLimit, dirsOnly, filesOnly, fetched, &total,
                        err)) {
      out = std::move(fetched);
      return true;
    }
    b.client.close();
  }
  return false;
}

bool doStats(Bridge& b, std::vector<std::pair<std::string, std::string>>& kv,
             std::string& err) {
  for (int attempt = 0; attempt < 2; ++attempt) {
    if (!ensureClient(b, err)) return false;
    if (b.client.stats(kv, err)) return true;
    b.client.close();
  }
  return false;
}

std::string unavailableText(const std::string& detail) {
  return "lsearchd is unavailable: " + detail +
         "; first full scan may be running or the daemon is not reachable; retry in ~10s";
}

DBusHandlerResult handleSearch(Bridge& b, DBusConnection* conn, DBusMessage* msg) {
  const char* query = nullptr;
  const char* sort = nullptr;
  const char* kind = nullptr;
  dbus_uint32_t limit = 0, offset = 0;
  DBusError derr;
  dbus_error_init(&derr);
  if (!dbus_message_get_args(msg, &derr, DBUS_TYPE_STRING, &query, DBUS_TYPE_UINT32, &limit,
                             DBUS_TYPE_UINT32, &offset, DBUS_TYPE_STRING, &sort,
                             DBUS_TYPE_STRING, &kind, DBUS_TYPE_INVALID)) {
    dbus_error_free(&derr);
    sendError(conn, msg, kErrInvalidArgs, "Search expects (s query, u limit, u offset, s sort, s kind)");
    return DBUS_HANDLER_RESULT_HANDLED;
  }
  // 不重实现校验：交给 MCP 的参数解析（控制字符/白名单/空查询/钳制/正则）。
  mcp::SearchArgs sa;
  std::string perr;
  if (!makeSearchArgs(query, limit, offset, sort, kind, sa, perr)) {
    sendError(conn, msg, kErrInvalidArgs, perr);
    return DBUS_HANDLER_RESULT_HANDLED;
  }
  const std::size_t n = mcp::overFetch(sa.offset, sa.limit);
  std::vector<SearchResult> fetched;
  std::string serr;
  if (!doSearch(b, sa, n, fetched, serr)) {
    sendError(conn, msg, kErrDaemonUnavailable, unavailableText(serr));
    return DBUS_HANDLER_RESULT_HANDLED;
  }
  // Index::search 并行提前终止时结果子集不确定；本页被填满且未到 cap 时改取 cap，
  // 以获得确定的全局前缀，保证相邻页不重叠（与 MCP 的 refetch 行为一致）。
  if (fetched.size() == n && n < mcp::kMaxFetch) {
    std::vector<SearchResult> full;
    std::string ferr;
    if (!doSearch(b, sa, mcp::kMaxFetch, full, ferr)) {
      sendError(conn, msg, kErrDaemonUnavailable, unavailableText(ferr));
      return DBUS_HANDLER_RESULT_HANDLED;
    }
    fetched = std::move(full);
  }
  mcp::Page page = mcp::paginate(fetched, sa.offset, sa.limit);
  mcp::applyCapTruncation(page, fetched.size());
  bool rebuilding = false;
  std::vector<std::pair<std::string, std::string>> kv;
  std::string sterr;
  if (doStats(b, kv, sterr)) {
    for (const auto& e : kv)
      if (e.first == "rebuilding") rebuilding = (e.second == "1");
  }
  replyString(conn, msg, mcp::searchPayload(page, rebuilding).dump());
  return DBUS_HANDLER_RESULT_HANDLED;
}

DBusHandlerResult handleStats(Bridge& b, DBusConnection* conn, DBusMessage* msg) {
  std::vector<std::pair<std::string, std::string>> kv;
  std::string err;
  if (!doStats(b, kv, err)) {
    sendError(conn, msg, kErrDaemonUnavailable, unavailableText(err));
    return DBUS_HANDLER_RESULT_HANDLED;
  }
  sendStats(conn, msg, mapStats(kv));
  return DBUS_HANDLER_RESULT_HANDLED;
}

DBusHandlerResult handleRebuild(Bridge& b, DBusConnection* conn, DBusMessage* msg) {
  const int64_t now = nowSeconds();
  if (!b.throttle.ready(now)) {
    sendError(conn, msg, kErrBusy,
              "rebuild was triggered less than 5s ago; retry in a few seconds");
    return DBUS_HANDLER_RESULT_HANDLED;
  }
  std::string err;
  bool ok = false;
  for (int attempt = 0; attempt < 2 && !ok; ++attempt) {
    if (!ensureClient(b, err)) break;
    ok = b.client.command("rebuild", err);
    if (!ok) b.client.close();
  }
  if (!ok) {
    sendError(conn, msg, kErrDaemonUnavailable, unavailableText(err));
    return DBUS_HANDLER_RESULT_HANDLED;
  }
  b.throttle.recordSuccess(now);
  replyEmpty(conn, msg);
  return DBUS_HANDLER_RESULT_HANDLED;
}

DBusHandlerResult bridgeMessage(DBusConnection* conn, DBusMessage* msg, void* userData) {
  if (dbus_message_get_type(msg) != DBUS_MESSAGE_TYPE_METHOD_CALL)
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
  auto* b = static_cast<Bridge*>(userData);
  const char* iface = dbus_message_get_interface(msg);
  const char* member = dbus_message_get_member(msg);
  if (!member) return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
  const std::string I = iface ? iface : "";
  const std::string M = member;

  if ((I.empty() || I == DBUS_INTERFACE_INTROSPECTABLE) && M == "Introspect") {
    replyString(conn, msg, kIntrospectXml);
    return DBUS_HANDLER_RESULT_HANDLED;
  }
  if ((I.empty() || I == DBUS_INTERFACE_PEER) && M == "Ping") {
    replyEmpty(conn, msg);
    return DBUS_HANDLER_RESULT_HANDLED;
  }
  if (!I.empty() && I != kInterface) return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

  if (M == "Version") {
    replyString(conn, msg, kBridgeVersion);
    return DBUS_HANDLER_RESULT_HANDLED;
  }
  if (M == "Search") return handleSearch(*b, conn, msg);
  if (M == "Stats") return handleStats(*b, conn, msg);
  if (M == "Rebuild") return handleRebuild(*b, conn, msg);
  return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

// 捕获会话总线断开信号：置位退出标志，确保下次调用能重新激活。
DBusHandlerResult bridgeFilter(DBusConnection*, DBusMessage* msg, void*) {
  if (dbus_message_is_signal(msg, DBUS_INTERFACE_LOCAL, "Disconnected")) {
    g_stop = 1;
    return DBUS_HANDLER_RESULT_HANDLED;
  }
  return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

}  // namespace

int main(int, char**) {
  signal(SIGPIPE, SIG_IGN);
  struct sigaction sa;
  std::memset(&sa, 0, sizeof(sa));
  sa.sa_handler = onSignal;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;  // 不加 SA_RESTART：让阻塞中的 poll 返回 EINTR
  sigaction(SIGTERM, &sa, nullptr);
  sigaction(SIGINT, &sa, nullptr);

  DBusError err;
  dbus_error_init(&err);
  DBusConnection* conn = dbus_bus_get(DBUS_BUS_SESSION, &err);
  if (!conn) {
    fprintf(stderr,
            "[lsearch-dbus] no session bus: %s; start a desktop session or run under "
            "dbus-run-session\n",
            dbus_error_is_set(&err) ? err.message : "unknown error");
    dbus_error_free(&err);
    return 1;
  }
  dbus_error_free(&err);
  dbus_connection_set_exit_on_disconnect(conn, FALSE);

  Bridge bridge;
  bridge.cfg = Config::load("");

  static const DBusObjectPathVTable vtable = {nullptr, bridgeMessage, nullptr, nullptr,
                                              nullptr, nullptr};
  if (!dbus_connection_register_object_path(conn, kObjectPath, &vtable, &bridge)) {
    fprintf(stderr, "[lsearch-dbus] failed to register object path %s\n", kObjectPath);
    dbus_connection_unref(conn);
    return 1;
  }
  dbus_connection_add_filter(conn, bridgeFilter, nullptr, nullptr);

  // DO_NOT_QUEUE：已有 owner 时直接失败而非排队等待（避免激活僵尸进程）。
  dbus_error_init(&err);
  int rc = dbus_bus_request_name(conn, kBusName, DBUS_NAME_FLAG_DO_NOT_QUEUE, &err);
  if (rc != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER) {
    // EXISTS / ALREADY_OWNER / 无效应答：安静退出，由现有 owner 服务。
    dbus_error_free(&err);
    dbus_connection_unref(conn);
    return 0;
  }
  dbus_error_free(&err);

  while (!g_stop) {
    // 先无阻塞地读写并派发已在队列/内核缓冲中的消息（可能含 Disconnected）。
    if (!dbus_connection_read_write_dispatch(conn, 0)) break;
    int fd = -1;
    if (!dbus_connection_get_unix_fd(conn, &fd) || fd < 0) {
      // 极少数情况无可用 fd：小睡后继续，仍响应信号。
      struct timespec ts{0, 200 * 1000 * 1000};
      nanosleep(&ts, nullptr);
      continue;
    }
    struct pollfd p{fd, POLLIN, 0};
    int pr = poll(&p, 1, 200);
    if (pr < 0) {
      if (errno == EINTR) continue;
      break;
    }
  }

  dbus_connection_flush(conn);
  dbus_bus_release_name(conn, kBusName, nullptr);
  // 只关闭自身到总线的连接；lsearchd 由 connectOrSpawn 以 setsid 独立运行，不受影响。
  dbus_connection_unref(conn);
  return 0;
}
