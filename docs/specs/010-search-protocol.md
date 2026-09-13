# Lsearch Spec 010 — 检索协议 v2：真 total/truncated + 路径子树过滤

Status: **Proposed**（已按 Oracle 设计评审修订：go-with-changes）

## Why
两条已入档技术债（Spec 006/008 已知限制）：
1. **分页不诚实**：daemon 的 `OK <count>` 只回传截断后条数（`total` 非真实命中数），
   `truncated` 计算了但从未传输；MCP 只能在"整页"时回退到 5 万 cap 重取以维持页稳定性。
2. **无法限定子树**：检索仅 basename 且作用于整个索引，代理/UI 无法"只在 /data 下找"。
评审另指出：新客户端对**仍运行中的旧 daemon**没有降级路径；cap 处 `total`/`has_more`
语义必须明确为下界/「无可继续页」；宽查询延迟需专门快速路径。

## What Changes
- `core/search`：`Index::search` 增加 `under` 前缀过滤与 outcome（`results/total/total_capped`）；
  早停点从请求的 `limit` 改为 **cap**（≤cap 时收集全部匹配 → total 精确、页稳定）；
  新增 **count-only** 快速路径（不物化、不排序）；`kCandidateCap` 支持测试注入
- `ipc`：新增 `search2 <limit> <dirs> <files> <sort> <under64> <query...>`，响应
  `OK <returned> <total> <total_capped>\n<结果行…>\nEND\n`；新增
  `count2 <dirs> <files> <under64> <query...>` → `OK <total> <total_capped>\n`；
  base64 编解码（`core/util`）；**能力协商**（`capabilities` 或扩展 `version`），
  新客户端连旧 daemon 时**降级**到 `search`（MCP 降级时保留现有 refetch 路径）
- `mcp`：`search_files` 增 `under`；`page` 增 `total`/`total_capped`/`total_is_lower_bound`/`hint`；
  `has_more` 改为精确；`truncated` 取自 daemon；移除重复常量 `kMaxFetch`；更正描述与 instructions
- `cli`：`--under`（`realpath` 规范化，失败 exit 2）；`--count` 走 `count2`（精确、免物化）
- 测试：core 单测（可注入 cap）+ IPC/MCP e2e + 修正旧 flaky 用例 + 性能证据
- 文档与计数同步

## Requirements

#### Requirement 1 — under 语义
`path == under || startsWith(path, under + "/")`（兄弟前缀不误命中：`/data` 不匹配 `/data2`）。
规范化：去掉尾部 `/`（但 `under = "/"` 表示全量）；`""`/`-` = 全量；拒绝控制字符（<0x20）。
MCP 必须绝对路径；CLI 用 `realpath` 规范化（失败 exit 2）；daemon 按**原始字节**比较 `e.path`
（大小写敏感、不解析 `..`/符号链接）。被 excludes/隐藏过滤掉的路径自然返回空。

#### Requirement 2 — 精确计数与 cap 语义
- 扫描在 `kCandidateCap` 处停止；`!total_capped` 时收集全部匹配（完整扫描）→ `total` 精确、
  页间稳定，`has_more = offset + returned < total`（精确）
- `total_capped = 1` 时：`total` 是**下界（≈cap）**，结果子集不保证跨调用稳定；
  `has_more = false` 仅表示"无可继续的 offset 页"，**完整性信号是 `total_capped`**
- 不承诺 >cap 的页稳定性（见非目标）

#### Requirement 3 — 协议、编解码与兼容
- `search` 命令与响应格式不变；`search2`/`count2` 为纯加法
- base64 严格解码：拒绝空白与非法字符、长度 ≤ 8192、解码后拒绝 NUL；
  违规 → `ERR bad under`
- **新客户端对旧 daemon 必须有降级路径**（能力协商，或 `ERR unknown command` 时回退 `search`；
  MCP 回退时保留现有 cap refetch），不得直接失败

#### Requirement 4 — CLI
`--under PATH`（规范化后以 base64 传给 daemon）；`--count` 使用 `count2`（精确且不物化结果）。
`--count` 退出语义保持 `total > 0 ? 0 : 1`；`total_capped` 时打印 cap 数值并在 stderr 提示
`>= N (capped)`，退出码仍 0。

#### Requirement 5 — MCP
- `under`：可选；非空、绝对路径、无 C0、长度 ≤ 4096；拒绝 CLI 的 `-` 哨兵；不要求存在
- `page` 新增 `total`/`total_capped`/`total_is_lower_bound`/`hint`（加法，不破坏既有调用者）；
  `has_more` 精确；`truncated` 取自 daemon 的 `total_capped`（不再用 `overFetch>=kMaxFetch` 启发式）
- capped 时 `hint` 给模型可操作建议（如 "more than 50000 matches; narrow the query or add under"）
- 更正 `mcp/protocol.cpp` 描述（"no path-subtree filter"）与 discover instructions（"user's home" → 实际 roots）

#### Requirement 6 — 性能与证据
提供 count-only 快速路径。证据须含（Release、真实索引、≥20 次、p50/p95）：
substring 宽查询 `p95 < 100ms`、窄查询 `< 30ms`、count-only `< 30ms`、旧 `search` 宽查询 `< 10ms`；
regex 宽查询 `< 250ms` **或**按 Spec 008 明确排除在"即时"目标外。若宽查询 >100ms，
证据必须给出 `under` 收窄后的数值。

#### Requirement 7 — 回归与测试
- core 单测（确定性）：under 精确/子树/无命中、`"/"`/`""`/`-`、尾斜杠规范化、兄弟前缀、
  大小写敏感、与 `re:`/glob/dirs-only 正交、精确计数、`offset+returned == total`、
  用**可注入 cap** 构造 >cap 用例
- IPC e2e：合法/非法 base64、超长、控制字符、缺字段 → `ERR bad under`；旧 `search` 响应字节不变
- MCP 增补：`under` 过滤、`page.total` 精确、`has_more` 边界、非法 `under` → `-32602`、cap 行为
- **Flaky 规则**：>cap 只断言 flags + membership，绝不比较顺序/身份；≤cap 与顺序参考全等比较；
  单测不做 timing/线程数断言；修正 `search_regex_paging_prefix`（改为新语义）
- 全部既有自测保持通过（`lsearch_tests`、`self-test.sh`、`self-test-mcp.sh`、`self-test-dbus.sh`）

## 非目标
- >cap 的稳定分页（由收窄查询/`under` 解决）；TUI/GUI 的 under 界面（其 `total` 仍为
  post-limit，属既有问题、非本次回退）；`index_stats` 分根计数（scope creep，`roots` 已暴露）；
  top-K 堆优化（可后续）
- fuzzy；多前缀；全路径名匹配（仍 basename）

## Scenario
Given 隔离索引含 `/a/dup.txt` 与 `/b/dup.txt`
When `lsearch --under /a -m dup`
Then 仅返回 `/a/dup.txt`；`--count` 精确
When MCP `search_files{query:"dup", under:"/a"}`
Then `page.total` 精确、`has_more` 精确、`total_capped=false`
When 命中 >cap
Then `total_capped=true`、`total` 为下界、`has_more=false`，`page` 含
`total_is_lower_bound` 与收窄/`under` 的 `hint`
When 新客户端连到**仍运行的旧 daemon**（无 `search2`）
Then 能力协商后降级到 `search`（MCP 保留 refetch），不失败
When `search2` 收到非法/超长 base64 或含控制字符的 under
Then `ERR bad under`
When `--count` 命中 >cap
Then 打印 cap 数值 + stderr `>= N (capped)`，退出 0

## Task
- [x] Oracle 设计评审并据其修订本规格
- [ ] `core/search`：under + outcome + count-only + 可注入 cap
- [ ] `core/util`/`ipc/proto`：base64 编解码（严格）
- [ ] `ipc`：`search2`/`count2`/`capabilities` + `Client::searchEx/countEx` + 降级
- [ ] `daemon`：三条命令的分发与响应
- [ ] `mcp`：`under` + page 字段 + 降级路径 + 描述更正
- [ ] `cli`：`--under`、`--count`(count2)
- [ ] 测试（含修正旧 flaky 用例）+ 性能证据 + 文档与计数同步

## Deliverable
`core/search.*`、`core/util.*`、`ipc/{proto,client}.*`、`daemon/daemon.cpp`、`mcp/*`、
`cli/main.cpp`、测试与文档

## Evidence
（未实现，无）
