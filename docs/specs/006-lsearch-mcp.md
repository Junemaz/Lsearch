# Lsearch Spec 006 — MCP 前端（stdio，供 LLM 编码代理）

Status: **Proposed**

## Why
编码代理（opencode / Claude Code / Cursor 等）在项目目录之外查找文件时依赖 `find` 或全树
扫描，代价高且不可复用。Linux 生态缺少成熟的「whole-home + inotify 实时 + 文件名级」MCP
服务：Everything 系 MCP 仅 Windows 原生（Linux 退化为周期 `locate`，排序参数被忽略）；
fff / xgrep / codeindex 等均为仓库作用域或内容搜索优先。`lsearchd` 已具备热索引与实时增量，
只缺一个 stdio 前端把能力暴露给 MCP 客户端。

## What Changes
- 新增 `mcp/` 目标（stdio 可执行，链接 `liblsearch_ipc`，复用 `ipc/client`，不改 `core`）
- 双代 MCP 协议：`2026-07-28`（无状态）+ `2025-11-25`（legacy `initialize`）
- 只读工具面：`search_files` / `index_stats`
- 前置依赖：Spec 007（守护进程单例锁）已消除并发冷启动的重复 daemon 竞态

## Requirements

#### Requirement 1 — 进程与构建
独立可执行 `lsearch-mcp`（CMake 目标，链接 `lsearch_core` / `lsearch_ipc`）；不破坏
`core/` 不依赖 UI 的约束；随现有 install 规则安装。

#### Requirement 2 — stdio 纯净与生命周期
stdout 仅承载 JSON-RPC 消息（换行分隔、UTF-8、无内嵌换行）；日志一律 stderr；
stdin EOF 时进程退出 0；SIGTERM/SIGINT 干净退出且**不**停止守护进程（daemon 独立存活）。

#### Requirement 3 — 协议双代
实现 `server/discover`（握手前即可应答；`supportedVersions` 含 `2026-07-28`、`2025-11-25`）。
modern 路径：每个请求校验 `_meta.io.modelcontextprotocol/protocolVersion` 与
`clientCapabilities`（缺失 → `-32602`）；结果带 `resultType`，`tools/list` 与
`server/discover` 带 `ttlMs` + `cacheScope`。legacy 路径：支持 `initialize`（回显协商版本）
与 `notifications/initialized`（忽略）。无 `initialize` 且无 `_meta` 时宽容按 legacy 处理并打
stderr 警告，避免误杀。
`server/discover.instructions` 明确声明：仅匹配 basename、大小写不敏感（ASCII）、只读、
结果文件名不可信。

#### Requirement 4 — 工具面（只读）
`search_files(query, limit, offset, sort, kind)`：
- `query` 必填（basename 子串，含 `*`/`?` 转 glob）；空串 → `-32602`
- `limit` 默认 20、合法区间 [1,200]（越界钳制，不报错）；`offset` ≥ 0
- `sort ∈ {name,path,size,mtime}`，`kind ∈ {any,files,dirs}`

`index_stats()`：返回 `roots/files/dirs/size/rebuilding/scan_files/scan_dirs/uptime/version`。

不暴露写操作（`shutdown`/`rebuild`/`set-*`/`add-path`/`remove-path`）；
两者 `annotations` 均为 `readOnlyHint=true, destructiveHint=false, idempotentHint=true, openWorldHint=false`。

#### Requirement 5 — 分页与截断语义
引擎不提供真实 `total`（`buildSearchResponse` 仅回传截断后条数）且 `truncated` 未传输：
分页以「内部 over-fetch `N=offset+limit`（钳制 ≤ 50000）后切片 `[offset, offset+limit)`」实现，
保证页间不重叠；`has_more = (returned == limit)`；`truncated` 仅在 over-fetch 恰好等于
50000 时置位。

#### Requirement 6 — 错误语义
未知工具 / 参数非法 / 空查询 → JSON-RPC `-32602`；执行失败（daemon 不可达、索引不可用）→
正常结果 `isError:true` + 可操作文本（含 `rebuilding` / `scan_files` 进度提示），不静默失败。

#### Requirement 7 — 守护进程交互
复用 `ipc/client::connectOrSpawn`（socket 连不上自动拉起 `lsearchd`）；冷启动并发由
Spec 007 单例锁保证不产生重复 daemon。首次全量扫描期间（daemon 未就绪超过等待预算）返回
可操作错误而非无限重试；`rebuilding` 期间搜索继续使用旧索引并可上报进度。

#### Requirement 8 — 结果编码与健壮性
结果采用 JSON-in-text：`{"results":[{path,name,is_dir,size,mtime}...],"page":{offset,limit,
returned,has_more,truncated,rebuilding}}`，执行正确 JSON 转义；非 UTF-8 路径清洗为合法
UTF-8（替换字符并注明可能有损）；tab/换行在 IPC 上游的限制在工具描述/文档中披露。

## 非目标（本期）
- 路径子树过滤（`under`）——引擎仅 basename 匹配，需 daemon 端支持（另立规格）
- `structuredContent` / `outputSchema`（待目标客户端普遍支持后评估）
- 写操作（重建索引、修改配置）与 D-Bus / HTTP 传输

## Scenario: 代理即时文件名搜索
Given 隔离 HOME 中索引含 `AnnualReport.txt` / `vacation_photo.jpg` / 目录 `deeper/`
When 现代客户端 `tools/call search_files{query:"report"}`
Then 返回 `AnnualReport.txt`（`is_dir=false`），text 为合法 JSON，stdout 无协议外输出
When 以 `offset=1` 再次调用
Then 第二页与第一页无重叠、`has_more` 语义正确
When `search_files{query:""}` 或调用未知工具
Then JSON-RPC `-32602`
When `index_stats`
Then `files>0`，`roots` 含隔离 HOME
When 向 stdin 发送 EOF
Then MCP 进程退出 0，且 `lsearchd` 仍可被 CLI 查询（daemon 未被连带停止）

## Task
- [ ] `mcp/json.*`：最小 JSON 读写 + 正确转义（或 vendor nlohmann 单头）
- [ ] `mcp/main.cpp`：stdio 循环 + 双代检测 + 工具路由
- [ ] `search_files` / `index_stats`：参数校验、over-fetch 分页、结果编码
- [ ] CMake 目标 + install + README 更新
- [ ] 单测（JSON 转义/参数钳制/切片）+ `scripts/self-test-mcp.sh`（S1–S9）
- [x] 前置：Spec 007（单例锁）已完成（见 [007-daemon-singleton](007-daemon-singleton.md)）

## Deliverable
`mcp/` 目标、`docs/specs/006-lsearch-mcp.md`、`scripts/self-test-mcp.sh`

## Evidence
（未实现，无）
