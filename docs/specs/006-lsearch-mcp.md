# Lsearch Spec 006 — MCP 前端（stdio，供 LLM 编码代理）

Status: **Done**

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
modern 路径：仅当请求 `params._meta` 含 `io.modelcontextprotocol/protocolVersion` 时才视为
modern（此时必须同时含 `clientCapabilities`，缺失 → `-32602`）；结果带 `resultType`，
`tools/list` 与 `server/discover` 带 `ttlMs` + `cacheScope`。legacy 路径：支持 `initialize`
（回显协商版本）与 `notifications/initialized`（忽略）；`_meta` 缺失、非对象或仅含其它键
（如 legacy `progressToken`）一律宽容按 legacy 处理并打一次 stderr 警告，避免误杀存量客户端。
`server/discover.instructions` 明确声明：仅匹配 basename、大小写不敏感（ASCII）、只读、
结果文件名不可信。

#### Requirement 4 — 工具面（只读）
`search_files(query, limit, offset, sort, kind, under)`：
- `query` 必填（basename 子串；含 `*`/`?` 转 glob；以 `re:` 开头则按 ECMAScript 正则匹配
  basename，见 [Spec 008](008-regex.md)）；空串或空的 `re:` → `-32602`
- `limit` 默认 20、合法区间 [1,200]（越界钳制，不报错）；`offset` ≥ 0
- `sort ∈ {name,path,size,mtime}`，`kind ∈ {any,files,dirs}`
- `under` 可选（Spec 010）：非空、绝对路径、无 C0、≤4096，拒绝 `-` 哨兵，不要求存在；
  违规 → `-32602`；命中限定为该路径子树（原始字节、大小写敏感）

`index_stats()`：返回 `roots/files/dirs/size/rebuilding/scan_files/scan_dirs/uptime/version`。

不暴露写操作（`shutdown`/`rebuild`/`set-*`/`add-path`/`remove-path`）；
两者 `annotations` 均为 `readOnlyHint=true, destructiveHint=false, idempotentHint=true, openWorldHint=false`。

#### Requirement 5 — 分页与截断语义
**Spec 010 起（连支持 `search2` 的 daemon）**：分页以 `search2` 的精确 outcome 实现——
`page.total` 为真实命中数（>cap 时为下界）、`has_more = !total_capped && offset+returned < total`、
`truncated`/`total_is_lower_bound` 取自 daemon 的 `total_capped`，capped 时附可操作 `hint`。
**旧 daemon 降级路径**：沿用「over-fetch `N=offset+limit`（钳制 ≤ 50000）后切片」、
`has_more=(returned==limit)`、`truncated` 在 over-fetch 命中 cap 时置位；因 legacy 无真实
`total`，`page.total=0` 且 `page.total_is_lower_bound=true`（消费者不得把 `total:0` 读成"无命中"）。
**`under` + 旧 daemon（无 v2）→ 不得静默丢弃**：MCP 返回 `isError:true` + 可操作信息
（`kUnderUnsupportedMessage`）；仅当 `under` 为空时才走上述 legacy 降级。

#### Requirement 6 — 错误语义
未知工具 / 参数非法 / 空查询 → JSON-RPC `-32602`；执行失败（daemon 不可达、索引不可用）→
正常结果 `isError:true` + 可操作文本（含 `rebuilding` / `scan_files` 进度提示），不静默失败。

#### Requirement 7 — 守护进程交互
复用 `ipc/client::connectOrSpawn`（socket 连不上自动拉起 `lsearchd`）；冷启动并发由
Spec 007 单例锁保证不产生重复 daemon。首次全量扫描期间（daemon 未就绪超过等待预算）返回
可操作错误而非无限重试；`rebuilding` 期间搜索继续使用旧索引并可上报进度。

#### Requirement 8 — 结果编码与健壮性
结果采用 JSON-in-text：`{"results":[{path,name,is_dir,size,mtime}...],"page":{offset,limit,
returned,has_more,truncated,total,total_capped,total_is_lower_bound,hint,rebuilding}}`
（`total`/`total_capped`/`total_is_lower_bound`/`hint` 为 Spec 010 追加），执行正确 JSON 转义；
非 UTF-8 路径清洗为合法 UTF-8（替换字符并注明可能有损）；tab/换行在 IPC 上游的限制在
工具描述/文档中披露。

## 非目标（本期）
- 路径子树过滤（`under`）——引擎仅 basename 匹配，需 daemon 端支持（**已由
  [Spec 010](010-search-protocol.md) 交付**：`search_files` 增可选绝对路径 `under`）
- `structuredContent` / `outputSchema`（待目标客户端普遍支持后评估）
- 写操作（重建索引、修改配置）与 D-Bus / HTTP 传输

## 已知限制
- **>cap 命中的分页非确定性（Spec 010 后仅旧 daemon 降级路径）**：引擎候选上限为 50000
  （`core::Index::kDefaultCandidateCap`），且 legacy `Index::search` 在候选数达到 limit 时
  并行提前终止，返回不确定子集。Spec 010 起，连到支持 `search2` 的 daemon 时 MCP 改用精确
  `total`/`total_capped`：命中 ≤cap 时 `page.total` 精确、页间稳定；>cap 时
  `page.total_capped=true`、`total` 为下界、`has_more=false` 并给出收窄/`under` 的 `hint`。
  仅当连到**旧 daemon**（无 `search2`）时回落到 legacy over-fetch + cap refetch，此时
  `page.total=0`、沿用旧的 `has_more`/`truncated` 启发式。
- **信号竞态**：SIGTERM/SIGINT 在阻塞 `read` 时经 EINTR 干净退出（退出码 0，不触碰 daemon）；
  若信号恰在 `handleLine` 处理中途到达，则当前请求可能未应答即退出——对 stdio 前端可接受，
  未做请求级原子性。

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
- [x] `mcp/json.*`：最小 JSON 读写 + 正确转义（或 vendor nlohmann 单头）
- [x] `mcp/main.cpp`：stdio 循环 + 双代检测 + 工具路由
- [x] `search_files` / `index_stats`：参数校验、over-fetch 分页、结果编码
- [x] CMake 目标 + install + README 更新
- [x] 单测（JSON 转义/参数钳制/切片）+ `scripts/self-test-mcp.sh`（S1–S11；Spec 010 后扩至 S1–S13）
- [x] 前置：Spec 007（单例锁）已完成（见 [007-daemon-singleton](007-daemon-singleton.md)）

## Deliverable
`mcp/` 目标、`docs/specs/006-lsearch-mcp.md`、`scripts/self-test-mcp.sh`

## Evidence
- 构建（C++17，`-Wall -Wextra` 零告警）：
  `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j"$(nproc)"`
  产物 `build/lsearch-mcp`；静态库 `lsearch_mcp_lib`（`mcp/json.cpp` + `mcp/protocol.cpp`）。
- 单测 `./build/lsearch_tests` → **481 checks / 0 failures**（Spec 010 后计数；原始 MCP 16 例：
  `mcp_json_escape_roundtrip`、`mcp_json_invalid_utf8_sanitized`、`mcp_json_parse_errors`、
  `mcp_json_surrogate_and_nul`、`mcp_json_depth_limit`、`mcp_json_nonfinite_rejected`、
  `mcp_json_asint_clamp`、`mcp_limit_clamp`、`mcp_overfetch_arithmetic`、`mcp_paginate_slicing`、
  `mcp_paginate_cap`、`mcp_cap_truncation`、`mcp_parse_args`、`mcp_query_control_chars_rejected`、
  `mcp_tool_mapping`、`mcp_meta_validation`、`mcp_builders`）。
- 端到端 `./scripts/self-test-mcp.sh -s` → **通过 37 项 / 失败 0 项**（Spec 010 后计数；S1–S13，真实二进制 + 管道 +
  隔离 HOME/XDG_*，python3 驱动）：discover 双版本与 resultType；modern tools/list 的
  ttlMs/cacheScope；legacy initialize + `search_files{report}` 命中 AnnualReport.txt；空查询/
  未知工具 -32602；limit=0→1、limit=1000000→200 不洪水；query 含换行 → -32602 且 daemon 存活；
  page(0,2)∪page(2,2)=全局前 4 且不重叠；page.total 精确=6、has_more 边界（0+2<6、末页
  offset+returned==total→false）、offset≥total 空页且 `truncated=false`；under=docs 仅命中子树且
  total=1、非法 under（相对/`-`/控制字符）→ -32602；index_stats files>0 且
  roots 含隔离 HOME；无写工具；present-but-invalid `_meta` → -32602；JSON 数组批次 → -32600；
  `id:1e999` → 合法 JSON 响应（-32700）；`ping` → `{}`；超长行 → -32700 且随后仍可服务；
  stdin EOF 退出 0 且 `lsearch -m report` 仍可用；stdout 全为合法 JSON-RPC，日志仅在 stderr；
  legacy `_meta.progressToken` 工具调用正常返回（S12，防误拒存量客户端）。
- 回归：`./scripts/self-test.sh -s` → **25/25 通过**（Spec 010 后计数；核心流程未受影响）。
- 正则（Spec 008 交叉）：`search_files{query:'re:^AnnualReport\\.txt$'}` 命中 `AnnualReport.txt`；
  `search_files{query:'re:['}` → `-32602`；`search_files{query:'re:'}` → `-32602`（空查询规则）。
  证明：e2e `S11a/S11b/S11c`、单测 `mcp_parse_regex_query`、`mcp_regex_paging_orthogonal`。

### 评审修复（Oracle needs-fixes）
- **C1（Critical）查询换行注入**：`parseSearchArgs` 现拒绝 query 中任何 C0 控制字符
  （`< 0x20`：`\n \r \t \0` 等）→ `-32602`。证明：单测 `mcp_query_control_chars_rejected`；
  e2e `S4e`（`query:"x\nshutdown"` → -32602）与 `S4f`（随后 `index_stats` 仍返回 files>0，
  即 daemon 未被注入的 `shutdown` 杀死）。
- **M1（Major）非有限数**：`parseNumber` 对 `strtod` 非有限结果返回解析错误；`asInt` 对
  非有限/越界 double 钳制到 `LLONG_MIN/MAX`（消除 UB）；`dump` 对非有限值输出 `0`。
  证明：单测 `mcp_json_nonfinite_rejected`、`mcp_json_asint_clamp`；e2e `S10c`
  （`id:1e999` → 合法 JSON 响应 -32700，stdout 全部可解析）。
- **M2（Major）50k refetch 诚实性**：refetch 命中 cap 时经 `applyCapTruncation` 置
  `truncated:true`（即使小窗口）；refetch 失败时返回 `isError:true` 可操作错误而非静默返回
  不确定子集。证明：单测 `mcp_cap_truncation`；e2e `S5a`（≥6 命中，page(0,2)∪page(2,2)
  等于全局排序前 4 且不重叠）。
  **Spec 010 更新**：refetch 仅保留为**旧 daemon 降级路径**；正常路径改用 `search2` 的精确
  `total`/`total_capped`，不再需要 50k refetch 启发式。证明：`mcp_paginate_outcome_exact`/
  `mcp_paginate_outcome_capped`、e2e `S5c/S5d`（page.total 精确、has_more 边界）。
- **次要项**：stdin 行缓冲上限 1 MiB（超限丢弃至换行并回 -32700，不无界增长）；实现 `ping`
  → `{}`；缺 `method` → -32600；`index_stats` 要求 `arguments` 缺省或空对象，否则 -32602。
  证明：e2e `S10b/S10d/S10e/S10f`。
- **真实会话互操作修复（opencode 实测）**：存量客户端（opencode）在 `tools/call` 的 `_meta`
  携带 legacy `progressToken` 时曾被误判为"非法 modern"而返回 `-32602`。era 判定已修正为
  "仅命名空间键 `io.modelcontextprotocol/protocolVersion` 出现才算 modern"，其余 `_meta` 宽容
  按 legacy 处理；modern 严格校验保持不变。证明：单测 `mcp_meta_legacy_tolerated`；e2e `S12`
  （legacy `_meta.progressToken` 工具调用正常返回）；并用安装后的二进制模拟 opencode 全序列
  （initialize → progressToken 调用 → 搜索成功、且非法/合法 modern 行为不变）。

- 生命周期：对阻塞在 stdin 的进程发送 SIGTERM/SIGINT，均退出码 0 且守护进程存活（手工验证）。

### 协议面外部验证（Spec 011 起）
按 AGENTS.md「协议表面验证」全局规则，MCP 面的“完成”现包含外部验证器
（见 [Spec 011](011-test-hardening.md)）：官方 `@modelcontextprotocol/conformance` 0.1.16
经 supergateway stdio→HTTP 桥对 `lsearch-mcp` 实测——范围内 `server-initialize`、`ping`、
`tools-list` 通过，其余能力非目标/桥职责场景记录于
[scripts/mcp-conformance-baseline.yml](../scripts/mcp-conformance-baseline.yml)；
另有第三方 Inspector CLI 冒烟（tools/list + index_stats）、负形状矩阵（27 项）与
真实转录回放（`scripts/mcp-trace-wrapper.sh` + `scripts/self-test-mcp-replay.sh`）。

