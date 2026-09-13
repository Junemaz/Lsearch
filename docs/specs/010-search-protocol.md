# Lsearch Spec 010 — 检索协议 v2：真 total/truncated + 路径子树过滤

Status: **Proposed**

## Why
两条已入档技术债（Spec 006/008 已知限制）：
1. **分页不诚实**：daemon 的 `OK <count>` 只回传截断后条数（`total` 非真实命中数），
   `truncated` 计算了但从未传输；MCP 只能在"整页"时回退到 5 万 cap 重取以维持页稳定性。
2. **无法限定子树**：检索仅 basename 且作用于整个索引，代理/UI 无法"只在 /data 下找"。

## What Changes
- `core/search`：`Index::search` 增加 `under` 前缀过滤与精确计数；返回 outcome
  （`results/total/total_capped/truncated`）
- `core/search`：**早停点从请求的 `limit` 改为 `kCandidateCap`（5 万）**——≤cap 时收集全部匹配
  （扫描完整索引），使 `total` 精确、页间稳定；>cap 时才早停并置 `truncated`
- `ipc`：新增命令 `search2`（旧 `search` 保留兼容）：
  `search2 <limit> <dirs> <files> <sort> <under64> <query...>`，响应
  `OK <returned> <total> <total_capped> <truncated>\n<结果行…>\nEND\n`
  `under64` = base64(路径前缀) 或 `-`（不限）；base64 新增于 `core/util` 或 `ipc/proto`
- `ipc/client`：新增 `searchEx(...)`（under + 精确 total/capped/truncated）；旧 `search` 行为不变
- `mcp`：`search_files` 增加 `under` 参数；改用 `searchEx`；`page` 输出精确
  `total`/`total_capped`/`has_more`/`truncated`，**移除 cap 回退重取**（≤cap 时已确定）
- `cli`：新增 `--under PATH`；`--count` 变为精确计数（至 cap）
- 测试：core 单测 + IPC/MCP e2e + 计数与文档同步

## Requirements

#### Requirement 1 — under 语义
仅收集 `path == under` 或 `startsWith(path, under + "/")` 的条目；`under` 为空或 `-` 不限；
`under = "/"` 等价不限；按字节前缀比较（与 excludes 同规则）。`under` 与子串/glob/`re:`
正交（先前缀过滤，后匹配）。

#### Requirement 2 — 精确计数与早停
- 扫描在 `kCandidateCap` 处停止；`total` = 实际匹配数（≤cap 精确），`total_capped` = 是否达到 cap
- `truncated` = `total_capped`（结果集被 cap 截断）
- `!total_capped` 时：全部匹配已收集、排序后切片，页间稳定；
  `has_more = offset + returned < total`（精确）
- `total_capped` 时：结果为任意 5 万子集，`has_more = false`，提示收窄查询或使用 `under`

#### Requirement 3 — 协议兼容与健壮
`search` 命令与响应格式不变；`search2` 为纯加法。`under64` 长度上限（如 8 KiB）；
base64 解码失败或超长 → `ERR bad under`；`under` 含控制字符 → `ERR bad under`。

#### Requirement 4 — MCP
`under` 参数（可选 string）：必须是绝对路径、拒绝 C0 控制字符；结果 `page` 含
`total`/`total_capped`；`has_more` 精确；`truncated` 直传；工具描述更新。
删除 `mcp/main.cpp` 的 50k 回退重取路径（改由 daemon 一次返回）。

#### Requirement 5 — 性能
非 regex 路径的每条目开销不变；`under` 在匹配前短路。宽查询（如单字母）会扫描至 cap——
Evidence 必须给出实测耗时（含 `under` 收窄后的对比）并确认打字交互仍即时（目标 <100ms）。

#### Requirement 6 — 回归
既有 `search`/MCP/CLI/TUI/GUI 行为不变；`lsearch_tests`、`self-test.sh`、`self-test-mcp.sh`、
`self-test-dbus.sh` 全绿；`self-test-mcp.sh` 增补 `under` 与 `total` 断言。

## 非目标
- TUI/GUI 的 under 图形界面（后续）；模糊匹配；多前缀
- cap 之上的稳定分页（>cap 直接 `truncated=true`，靠收窄查询/`under` 解决）
- 全路径匹配（仍 basename）

## Scenario
Given 隔离索引含 `/a/dup.txt` 与 `/b/dup.txt`
When `lsearch --under /a -m dup`
Then 仅返回 `/a/dup.txt` 且 `--count` 为精确值
When MCP `search_files{query:"dup", under:"/a"}`
Then `page.total` 精确、`has_more` 精确；同页字段含 `total_capped=false`
When 构造 >cap 命中（如 `query:"."` 或宽通配）
Then `truncated=true`、`has_more=false`（不再回退重取）
When `search2` 收到非法/超长 base64
Then `ERR bad under`
When 旧客户端调用 `search`
Then 行为与响应格式不变

## Task
- [ ] Oracle 设计评审（协议扩展方式、早停语义、性能取舍）
- [ ] `core/search`：under + 精确计数 + outcome；`core/util` 或 `ipc/proto` 增加 base64
- [ ] `ipc`：daemon `search2` 处理 + `Client::searchEx`
- [ ] `mcp`：`under` 参数 + 移除回退重取 + page 字段
- [ ] `cli`：`--under`、精确 `--count`
- [ ] 单测（under 边界/精确计数/cap）+ e2e（IPC/MCP）+ 文档与计数同步

## Deliverable
`core/search.*`、`ipc/{proto,client}.*`、`daemon/daemon.cpp`、`mcp/*`、`cli/main.cpp`、测试与文档

## Evidence
（未实现，无）
