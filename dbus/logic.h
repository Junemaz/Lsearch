#pragma once
// lsearch-dbus 纯逻辑层：D-Bus 调用参数映射/校验、stats 类型化、Rebuild 节流。
// 本层不含任何 libdbus 调用与 I/O，可在无总线的单测环境覆盖全部边界。
#include "mcp/protocol.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace lsearch {
namespace dbusbridge {

inline constexpr int64_t kRebuildCooldownSeconds = 5;

// stats 值 → D-Bus variant 的显式类型描述（避免把 roots 等字符串误当数字）。
struct StatVariant {
  enum class Type { U32, U64, Bool, Str };
  std::string key;
  Type type = Type::U32;
  uint32_t u32 = 0;
  uint64_t u64 = 0;
  bool boolean = false;
  std::string str;
};

// 把 D-Bus Search(s query, u limit, u offset, s sort, s kind) 映射为 mcp::SearchArgs。
// 复用 mcp::parseSearchArgs：控制字符拒绝、sort/kind 白名单、空查询拒绝、limit 钳制、
// 正则校验——桥接不重复实现这些规则。校验失败返回 false 并填 err。
bool makeSearchArgs(const std::string& query, uint32_t limit, uint32_t offset,
                    const std::string& sort, const std::string& kind,
                    mcp::SearchArgs& out, std::string& err);

// daemon stats 的 key=value 列表 → 固定键序的类型化变体。
// 类型映射：files/dirs/scan_files/scan_dirs → U32；size/uptime → U64；
// rebuilding → Bool；roots → Str。缺失键按 0/空/false 处理。
std::vector<StatVariant> mapStats(
    const std::vector<std::pair<std::string, std::string>>& kv);

// Rebuild 节流：仅在上次**成功**触发后的冷却窗口内拒绝；失败不占用冷却名额。
class RebuildThrottle {
 public:
  explicit RebuildThrottle(int64_t cooldownSeconds = kRebuildCooldownSeconds);
  bool ready(int64_t nowSeconds) const;
  void recordSuccess(int64_t nowSeconds);
  int64_t cooldownSeconds() const { return cooldown_; }

 private:
  int64_t cooldown_;
  bool hasLast_ = false;
  int64_t last_ = 0;
};

}  // namespace dbusbridge
}  // namespace lsearch
