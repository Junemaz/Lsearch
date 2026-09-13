# Lsearch Spec 008 — 正则匹配（V3）

Status: **Done**

## Why
子串与通配符（`*` `?`）表达能力有限：无法表达字符类、重复次数与锚点（如
`^report-\d{4}\.(pdf|txt)$`）。Everything 同样提供正则检索；引擎当前按 basename 匹配，
增加第三种匹配模式即可贯通全部前端（IPC 无需新命令，仅查询字符串前缀语义）。

## What Changes
- `core/search`：查询以 `re:` 开头 → 正则模式（`std::regex` ECMAScript，大小写不敏感，
  `regex_search` 任意位置匹配）；`re:` 判定优先于 `*`/`?` 通配符判定
- `core/search`：新增 `validateQuery(query, err)`，供守护进程/前端做非法正则校验
  （与匹配使用同一编译路径）
- `daemon`：非法正则回 `ERR bad regex: ...`（不再静默空结果）；CLI/TUI/GUI 以错误呈现
- `mcp`：`search_files` 对非法正则返回 `-32602`；工具描述补充 `re:` 前缀
- `tui`：正则模式下关闭子串高亮（避免误高亮）
- 测试：core 单测 + `self-test.sh` 冒烟 + `self-test-mcp.sh` 用例；文档与计数同步

## Requirements

#### Requirement 1 — 触发与语义
仅当查询以精确前缀 `re:` 开头时进入正则模式；其后字符串为 ECMAScript 正则，以
`std::regex_search` 对 basename 做**大小写不敏感**（ASCII）匹配。非 `re:` 查询行为完全
不变（子串；含 `*`/`?` 转 glob）。正则模式与 `dirs_only/files_only`、排序、limit/offset、
候选上限的全部既有语义正交。

#### Requirement 2 — 空模式与非法模式
- `re:` 后为空（或全空白）→ 视为空查询：`Index::search` 返回空结果；MCP 按空查询规则
  返回 `-32602`
- 非法正则（语法错误）→ 不得静默返回空结果：daemon 以 `ERR bad regex: ...` 响应；
  CLI 打印错误并以退出码 2 结束；TUI 状态栏提示；MCP 返回 `-32602`
- 校验使用与匹配相同的编译路径（`validateQuery`），避免"校验通过但匹配失败"

#### Requirement 3 — 性能与安全
- 正则**每次 search 调用仅编译一次**（禁止 per-entry 编译）；沿用并行分块扫描与
  50000 候选上限
- 匹配须用**原始 pattern + `std::regex::icase`**（不得对 pattern 做小写化——会改变
  `\D`/`\W`/`\S` 等转义语义）
- **回溯风险（明确接受，不作缓解承诺）**：实现基于回溯式 `std::regex`；灾难性回溯
  **不受 basename 长度或 50000 候选上限约束**——候选上限限制的是「条目数」而非
  「单次匹配耗时」。例如约 30 字节的 basename 配合 `re:(a+)+$` 即可让单次搜索超过
  12 秒，约 40 字节近似无界。该本地 DoS 在 V3 被**明确接受**（单用户桌面 + 本地 MCP，
  输入可信）；后果与未来修复方向见 `## 已知限制`。

#### Requirement 4 — 兼容与不变量
- 既有查询（子串/glob）行为与性能不回退；`core` 不新增外部依赖（标准库 `std::regex`）
- TUI 高亮在正则模式降级为不高亮（文档披露）；IPC 协议不变（仅查询字符串前缀语义）
- 全部既有测试保持通过（`lsearch_tests`、`self-test.sh`、`self-test-mcp.sh`）

## 非目标
- PCRE 全特性（仅 ECMAScript 子集）；全路径正则（仍 basename）；`--regex` 专用 CLI 旗标
  （统一用 `re:` 前缀）；正则命中高亮

## 已知限制
- **灾难性回溯（本地 DoS，V3 明确接受）**：`std::regex` 为回溯引擎，病态模式
  （如 `re:(a+)+$`）的单次匹配耗时**不受 basename 长度上限或 50000 候选上限约束**
  （上限限制条目数，不限制每次匹配耗时）。具体后果：
  - 搜索线程可能被无限期占满；期间该线程持有索引 `shared_lock`，`applyWatch` 的
    `unique_lock` 会被阻塞，inotify 事件在内核缓冲区堆积、可能溢出 → **索引可能漂移**；
  - `lsearchd --shutdown` 仍能回复 `OK`，但卡在不可中断匹配中的工作线程无法回收，
    进程可能无法及时退出。
  未来修复（若工具将来服务不可信/多用户输入，应单独立规格）：改用线性时间引擎
  （如 RE2）或强模式复杂度守卫。
- **正则双重编译（接受，不优化）**：daemon 每次 search 先 `validateQuery` 编译一次、
  匹配时再编译一次；MCP 一页仅在**旧 daemon 降级路径**走 refetch 时可达 4 次编译
  （Spec 010 后正常路径用 `search2` 单次查询，无 refetch）。
- **TUI 正则不发光高亮（接受，不改）**：正则模式下关闭子串高亮以避免误高亮，且不实现
  正则命中高亮。
- **匹配期异常已防护**：worker 内 `regex_search` 已用 try/catch 包裹，`std::regex_error`
  不会逸出线程触发 `std::terminate`；异常按「本条不命中」处理并继续搜索。

## Scenario: 正则检索
Given 隔离 HOME 索引含 `AnnualReport.txt`、`AnnualReport2026.txt`、`vacation_photo.jpg`
When `lsearch -m 're:^AnnualReport\d{4}\.txt$'`
Then 仅命中 `AnnualReport2026.txt`
When `lsearch -m 're:ANNUALREPORT'`（大小写不敏感）
Then 命中两个 AnnualReport* 文件
When `lsearch -m 're:['`（非法）
Then 退出码 2，stderr 含 `bad regex`；守护进程仍可用（后续查询正常）
When MCP `search_files{query:'re:['}`
Then `-32602`
When `lsearch -m '*.jpg'` / `lsearch -m report`（既有语法）
Then 行为与引入正则前一致

## Task
- [x] `core/search`：`validateQuery` + 正则模式接入（编译一次/查询）
- [x] `daemon`：非法正则 `ERR` 路径
- [x] `mcp`：校验 → `-32602` + 工具描述更新
- [x] `tui`：正则模式关闭高亮
- [x] 单测 + `self-test.sh`/`self-test-mcp.sh` 用例 + 计数同步
- [x] 文档：README（快速开始）与规格清单、Spec 006 交叉引用

## Deliverable
`core/search.*`、`daemon/daemon.cpp`、`mcp/protocol.cpp`、`tui/main.cpp`、测试与文档

## Evidence
- 构建（C++17，`-Wall -Wextra` 零告警）：
  `cmake --build build -j"$(nproc)"`（强制重编改动文件后 grep warning/error 为空）。
- 单测 `./build/lsearch_tests` → **481 checks / 0 failures**（首版 218 → 237 → Oracle 修复后 256
  → 测试去 flaky 后 263 → MCP 互操作修复后 267 → D-Bus 桥接（Spec 009）后 334 → Spec 010 后 481）。
  新增/相关用例：`search_regex_anchored`（`re:^AnnualReport\d{4}\.txt$` 仅命中
  `AnnualReport2026.txt`）、`search_regex_case_insensitive`（`re:ANNUALREPORT` 命中 2 个）、
  `search_validate_query`（`re:[` 返回 false + 非空 message；空/空白 `re:` 与非 `re:` 查询合法）、
  `search_regex_empty_pattern`（`re:`/`re:   ` → 空结果）、`search_star_still_glob_not_regex`
  （`*.p?f` 仍走 glob 命中 `report.pdf`）、`search_regex_pattern_not_lowercased`
  （`re:\W` 不命中 `ABC`、`re:\w` 命中——锁定「pattern 不小写化」不变量）、
  `search_regex_non_ascii_and_icase`（`re:^报告\.txt$` 命中 UTF-8 basename；`re:report`
  icase 命中 `REPORT`）、`search_regex_paging_prefix`（`re:.` 经 `searchEx`：匹配数 ≤cap 时
  `total` 精确、页稳定，`limit<匹配数` 返回**确定前缀**；Spec 010 已由旧「不确定子集」语义改写）、
  `mcp_parse_regex_query`（MCP 空 `re:` / 非法 `re:[` → 拒绝）、
  `mcp_regex_paging_orthogonal`（cap 取全量前缀经 `paginate` 切片 == 全量排序前缀）。
- 稳定性：**独立复跑发现 2 个 flaky 用例**（`search_regex_paging_prefix`、
  `mcp_regex_paging_orthogonal` 误设「匹配数 > limit 时引擎返回确定前缀」——与已知限制矛盾）。
  当时改为只断言确定性语义（limit≥匹配数时排序一致；limit<匹配数时仅验证「条数=limit 且全为
  匹配集子集」；MCP 侧按 cap 取全量再 `paginate`），随后 **20 次连续运行 263/0 全部通过**。
  **Spec 010 再修订**：`search_regex_paging_prefix` 改用 `searchEx`，断言精确 `total` 与确定前缀。
- 端到端 `./scripts/self-test.sh -s` → **通过 25 项 / 失败 0 项**（Spec 010 后计数；首版 19 → 21 → 22）：
  `re:^AnnualReport\.txt$` 仅输出隔离 HOME 的 `AnnualReport.txt`；`re:[` → 退出码 2 +
  stderr 含 `bad regex`，随后普通查询仍命中（守护进程存活）；空正则 `re:`/`re:   ` 视为
  空查询（退出码 1，非错误 2）。
- 端到端 `./scripts/self-test-mcp.sh -s` → **通过 37 项 / 失败 0 项**（Spec 010 后计数；首版 25 → 27 → 28 → 29）：
  `S11a` `search_files{query:'re:^AnnualReport\.txt$'}` 返回 `AnnualReport.txt`；
  `S11b` `search_files{query:'re:['}` → `-32602`；`S11c` `search_files{query:'re:'}` → `-32602`。
- 回归：既有子串/glob/排序/分页/单例锁/关闭清理用例全部保持通过（见上两条冒烟计数）。
- 关键实现点：检测顺序 `re:` → `*`/`?` → 子串；正则从原始 `query.substr(3)` 编译
  `std::regex::ECMAScript | std::regex::icase`（不对 pattern 小写化），对原始
  `en.e.name` 做 `regex_search`，每次 search 仅编译一次并在并行 worker 间共享只读实例；
  **worker 内 `regex_search` 已 try/catch**，匹配期 `std::regex_error` 不逸出线程
  （不会 `std::terminate` 崩溃守护进程），按「本条不命中」继续；编译失败防御性返回空结果，
  `validateQuery` 为主闸门（daemon `ERR bad regex`、MCP `-32602`、CLI 退出码 2、TUI 状态栏）。
- **威胁模型接受（V3）**：`re:` 病态模式导致的本地 DoS 被**有意识地接受**（R3 已删除
  「basename 长度有限 + 候选上限缓解」这一错误表述）。依据：`lsearchd` 默认仅索引本机
  用户家目录、socket 权限 0600、调用方与桌面用户同处一个信任域（CLI/TUI/GUI 与本地 MCP），
  不面向不可信或多用户输入。若未来接入不可信输入，应将「线性时间引擎（如 RE2）或强模式
  复杂度守卫」单独立为规格，而非在 V3 内修补。
