#include "dbus/logic.h"

#include <cstdlib>

namespace lsearch {
namespace dbusbridge {

namespace {

std::string kvGet(const std::vector<std::pair<std::string, std::string>>& kv,
                  const std::string& key) {
  for (const auto& e : kv)
    if (e.first == key) return e.second;
  return std::string();
}

unsigned long long kvNum(const std::vector<std::pair<std::string, std::string>>& kv,
                         const std::string& key) {
  return std::strtoull(kvGet(kv, key).c_str(), nullptr, 10);
}

}  // namespace

bool makeSearchArgs(const std::string& query, uint32_t limit, uint32_t offset,
                    const std::string& sort, const std::string& kind,
                    mcp::SearchArgs& out, std::string& err) {
  mcp::Json args = mcp::Json::object();
  args.set("query", mcp::Json::str(query));
  args.set("limit", mcp::Json::integer(static_cast<long long>(limit)));
  args.set("offset", mcp::Json::integer(static_cast<long long>(offset)));
  args.set("sort", mcp::Json::str(sort));
  args.set("kind", mcp::Json::str(kind));
  return mcp::parseSearchArgs(args, out, err);
}

std::vector<StatVariant> mapStats(
    const std::vector<std::pair<std::string, std::string>>& kv) {
  std::vector<StatVariant> out;
  auto push = [&out](StatVariant v) { out.push_back(std::move(v)); };

  StatVariant roots;
  roots.key = "roots";
  roots.type = StatVariant::Type::Str;
  roots.str = kvGet(kv, "roots");
  push(std::move(roots));

  for (const char* key : {"files", "dirs", "scan_files", "scan_dirs"}) {
    StatVariant v;
    v.key = key;
    v.type = StatVariant::Type::U32;
    v.u32 = static_cast<uint32_t>(kvNum(kv, key));
    push(std::move(v));
  }

  StatVariant size;
  size.key = "size";
  size.type = StatVariant::Type::U64;
  size.u64 = kvNum(kv, "size");
  push(std::move(size));

  StatVariant rebuilding;
  rebuilding.key = "rebuilding";
  rebuilding.type = StatVariant::Type::Bool;
  rebuilding.boolean = (kvGet(kv, "rebuilding") == "1");
  push(std::move(rebuilding));

  StatVariant uptime;
  uptime.key = "uptime";
  uptime.type = StatVariant::Type::U64;
  uptime.u64 = kvNum(kv, "uptime");
  push(std::move(uptime));

  return out;
}

RebuildThrottle::RebuildThrottle(int64_t cooldownSeconds)
    : cooldown_(cooldownSeconds < 0 ? 0 : cooldownSeconds) {}

bool RebuildThrottle::ready(int64_t nowSeconds) const {
  if (!hasLast_) return true;
  return (nowSeconds - last_) >= cooldown_;
}

void RebuildThrottle::recordSuccess(int64_t nowSeconds) {
  last_ = nowSeconds;
  hasLast_ = true;
}

}  // namespace dbusbridge
}  // namespace lsearch
