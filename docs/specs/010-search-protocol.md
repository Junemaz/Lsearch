# Lsearch Spec 010 — 检索协议 v2：真 total/truncated + 路径子树过滤

Status: **Done**（已按 Oracle 设计评审修订：go-with-changes）

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
**连续斜杠折叠**：`"//"`/`"///"` 规范化后等同于 `"/"` = **全量**（不是"仅根目录"）——已明确
选定此行为并记录；调用方不应依赖"空 under 与 `//` 不同"。
MCP 必须绝对路径；CLI 用 `realpath` 规范化（失败 exit 2）；daemon 按**原始字节**比较 `e.path`
（大小写敏感、不解析 `..`/符号链接）。被 excludes/隐藏过滤掉的路径自然返回空。

#### Requirement 2 — 精确计数与 cap 语义
- 扫描在候选上限处停止；`!total_capped` 时收集全部匹配（完整扫描）→ `total` 精确、
  页间稳定，`has_more = offset + returned < total`（精确）
- `total_capped = 1` 时：`total` 是**下界（≈cap）**，结果子集不保证跨调用稳定；
  `has_more = false` 仅表示"无可继续的 offset 页"，**完整性信号是 `total_capped`**
- **恰好 == cap 不可区分**：第 cap 个匹配后是否还有更多无法在不继续扫描的前提下判断，
  因此"匹配数恰好等于 cap"也会报告 `total_capped=1`、`total=cap`。故 `hint` 用
  **"at least N matches"**（而非 "more than N"）；这是诚实的下界表述，已记录为已知歧义
- 不承诺 >cap 的页稳定性（见非目标）

#### Requirement 3 — 协议、编解码与兼容
- `search` 命令与响应格式不变；`search2`/`count2` 为纯加法
- base64 严格解码：拒绝空白与非法字符、长度 ≤ 8192、解码后拒绝 NUL；
  违规 → `ERR bad under`
- **新客户端对旧 daemon 必须有降级路径**（能力协商，或 `ERR unknown command` 时回退 `search`；
  MCP 回退时保留现有 cap refetch），不得直接失败
- **但 `under` 不得被静默丢弃**：当调用方请求**非空 `under`** 而 daemon 无
  `search2`/`count2` 时，`Client::searchEx`/`countEx` 必须**显式失败**并返回可操作信息
  （`kUnderUnsupportedMessage`："running daemon does not support 'under' … restart or upgrade
  lsearchd"）；CLI 打印 stderr 并退出 2，MCP 返回 `isError:true`。仅当 `under` 为空时走静默
  legacy 降级。理由：legacy `search` 无子树过滤，静默回退会返回请求子树**之外**的结果（正确性
  缺陷）。
- `Client::supportsV2()` 仅在**确定性**结论时缓存（capabilities 成功、或明确
  `ERR unknown command`）；瞬时故障（超时/传输错误）不缓存，下次调用重试。

#### Requirement 4 — CLI
`--under PATH`（规范化后以 base64 传给 daemon）；`--count` 使用 `count2`（精确且不物化结果）。
`--count` 退出语义保持 `total > 0 ? 0 : 1`；`total_capped` 时打印 cap 数值并在 stderr 提示
`>= N (capped)`，退出码仍 0。
**快速路径**：无 `--under` 且非 `--count` 时，CLI 走 legacy `Client::search`（请求 limit 早停），
保持既有即时体验；仅当设置 `--under` 时改用 `searchEx`。`--under` 遇旧 daemon → 退出 2 + 可操作错误
（绝不静默忽略）。

#### Requirement 5 — MCP
- `under`：可选；非空、绝对路径、无 C0、长度 ≤ 4096；拒绝 CLI 的 `-` 哨兵；不要求存在
- `page` 新增 `total`/`total_capped`/`total_is_lower_bound`/`hint`（加法，不破坏既有调用者）；
  `has_more` 精确；`truncated` 取自 daemon 的 `total_capped`（不再用 `overFetch>=kMaxFetch` 启发式）
- capped 时 `hint` 给模型可操作建议（**"at least 50000 matches; narrow the query or add under"**；
  用 "at least" 而非 "more than" 以覆盖"恰好 ==cap"不可区分的情形）
- **`under` + 旧 daemon（无 v2）**：返回 `isError:true` + 同一个 `kUnderUnsupportedMessage`，
  绝不静默丢弃 `under`；仅 `under` 为空时才降级到 legacy 分页
- **legacy 降级页**：`page.total=0` 为"未知"，故置 `total_is_lower_bound=true`，消费者不得把
  `total:0` 读作"无命中"
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
- IPC e2e：合法/非法 base64、超长、控制字符、缺字段 → `ERR bad under`；旧 `search` 响应字节不变；
  旧 daemon stub：无 under 静默降级成功、有 under（CLI 搜索/计数、MCP）显式失败
- client 单测（stub daemon）：`searchEx`/`countEx` 非空 under + 无 v2 → 失败 + 可操作信息；
  空 under → legacy 成功；`supportsV2` 确定性缓存 vs 瞬时故障重试
- MCP 增补：`under` 过滤、`page.total` 精确、`has_more` 边界、非法 `under` → `-32602`、cap 行为、
  legacy 页 `total_is_lower_bound=true`、`hint` 用 "at least"
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
When 命中 >cap（含恰好 ==cap 不可区分的情形）
Then `total_capped=true`、`total` 为下界、`has_more=false`，`page` 含
`total_is_lower_bound` 与 "at least N …" 的收窄/`under` `hint`
When 新客户端连到**仍运行的旧 daemon**（无 `search2`）且未请求 `under`
Then 能力协商后降级到 `search`（MCP 保留 refetch，且 legacy 页 `total_is_lower_bound=true`），不失败
When 新客户端/CLI/MCP 请求 `under` 但旧 daemon 无 `search2`/`count2`
Then **显式失败**（CLI 退出 2、MCP `isError:true` + 可操作信息），绝不返回子树外结果
When `search2` 收到非法/超长 base64 或含控制字符的 under
Then `ERR bad under`
When `--count` 命中 >cap
Then 打印 cap 数值 + stderr `>= N (capped)`，退出 0

## Task
- [x] Oracle 设计评审并据其修订本规格
- [x] `core/search`：under + outcome + count-only + 可注入 cap
- [x] `core/util`/`ipc/proto`：base64 编解码（严格）
- [x] `ipc`：`search2`/`count2`/`capabilities` + `Client::searchEx/countEx` + 降级
- [x] `daemon`：三条命令的分发与响应
- [x] `mcp`：`under` + page 字段 + 降级路径 + 描述更正
- [x] `cli`：`--under`、`--count`(count2)
- [x] 测试（含修正旧 flaky 用例）+ 性能证据 + 文档与计数同步

## Deliverable
`core/search.*`、`core/util.*`、`ipc/{proto,client}.*`、`daemon/daemon.cpp`、`mcp/*`、
`cli/main.cpp`、测试与文档

## Evidence

### 构建（C++17，`-Wall -Wextra` 零告警）
```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"        # 全绿，无 warning/error
```

### 单元测试：481 checks / 0 failures（Spec 009 基线 334 → 首版 431 → Oracle 修复后 481）
```
./build/lsearch_tests
```
新增/修正用例：
- `search_ex_under_exact_and_subtree`、`search_ex_under_sibling_guard`（`/data` 不匹配
  `/data2`、`/datax`）、`search_ex_under_no_match`、`search_ex_under_all_variants`
  （`""`/`"/"`/`"-"` 等全量）、`search_ex_under_trailing_slash`、`search_ex_under_case_sensitive`
  （`/data` 不命中 `/Data`）、`search_ex_under_orthogonal`（与 `re:`/glob/dirs-only/files-only 正交）、
  `search_ex_offset_returned_total`、`search_ex_cap_flags_membership`（注入 cap=3 的 >cap 用例，
  仅断言 flags + membership）、`count_ex_exact`、`count_ex_cap`、`base64_roundtrip_and_strict`
  （`Zg==`/`Zm8=`/`Zm9v` 编码、空白/非法字符/`====`/`AB==` 非规范填充/超长/解码含 NUL 全拒）、
  `mcp_parse_under_args`（合法/相对/`-`/空/含 NUL/超长/类型错误）、`mcp_paginate_outcome_exact`
  （`has_more` 边界）、`mcp_paginate_outcome_capped`（`total_is_lower_bound` + `hint` 含 "at least"）。
- **修正旧 flaky 用例** `search_regex_paging_prefix`：旧断言编码「limit<匹配数返回不确定子集」，
  已改为新语义——匹配数 ≤cap 时 `searchEx` 收集全部匹配（`total` 精确、页稳定），
  `limit<匹配数` 返回排序后的**确定前缀**。
- **Oracle 修复新增**：`search_ex_exact_cap_conservative`（注入 cap=4、恰好 4 匹配 → 保守
  `total_capped=true`、`total=4`，仅断言 flags+membership）、`search_ex_under_all_variants` 增
  `//`/`///`（折叠为全量）。client 单测（假 daemon stub，`tests/test_client.cpp`）：
  `client_search_ex_under_requires_v2`、`client_count_ex_under_requires_v2`（非空 under + 无 v2 →
  false + "does not support 'under'"）、`client_search_ex_no_under_legacy_ok`、
  `client_count_ex_no_under_legacy_ok`（空 under → legacy 成功）、
  `client_under_error_after_v2_confirmed_absent`（已确认无 v2 后 under 直接报错，不再往返）、
  `client_supports_v2_definitive_negative_cached`（确定性结论缓存）、
  `client_supports_v2_transient_retry`（瞬时故障不缓存、下次重试）、
  `client_supports_v2_positive_search2_count2`（正常路径）。

### IPC 协议 e2e：18/18
```
./scripts/self-test-ipc.sh -s     # 通过 18 项 / 失败 0 项（P1–P18）
```
`capabilities` 含 `search/search2/count2`；`search2` 无 under `total=2`、under=docs `total=1`；
`count2` 全量 `OK 2 0`、under `OK 1 0`；非法 base64 / 超长(>8192) / 解码含控制字符 / 缺字段
→ `ERR bad under`；非法正则 → `ERR bad regex`；旧 `search` 响应 `OK <count>` + 行 + `END` 字节不变。
**旧 daemon stub（P14–P18）**：普通 `search` 静默降级成功（P14）；`--under` 搜索（P15）与
`--under --count`（P16）→ 退出 2 + "does not support 'under'"；MCP `under` → `isError:true`（P17）；
MCP 无 under → 成功且 legacy 页 `total_is_lower_bound=true`（P18）。

### CLI `--under` / `--count`（实测 317511 条索引）
```
./build/lsearch -m --count a                       # stdout: 50000 ; stderr: >= 50000 (capped) ; rc=0
./build/lsearch -m --under /home/code/Lsearch --count lsearch   # 88（精确、免物化）
./build/lsearch -m --under /no/such/path dup       # exit 2（realpath 失败）
```
**快速路径**：无 `--under`/`--count` 的普通搜索走 legacy `Client::search`（请求 limit 早停）；
仅设 `--under` 时改用 `searchEx`。隔离自测 `self-test.sh` 亦覆盖 `--under` 计数（全量 2 / docs 子树 1）、
子树路径输出与失败退出码 2。

### 降级：新客户端 → 旧 daemon（实测在本机仍在运行的旧 `lsearchd` 上，只读查询）
```
# 旧 daemon 原始应答
capabilities / search2 / count2  =>  ERR unknown command
# 无 under：静默降级（Spec R3）
XDG_RUNTIME_DIR=/run/user/0 HOME=/root ./build/lsearch -m --count lsearch   # => 107, rc=0
XDG_RUNTIME_DIR=/run/user/0 HOME=/root ./build/lsearch -m -l 3 lsearch      # => 正常返回路径
# 有 under：显式失败，绝不返回子树外结果（Oracle MAJOR 修复）
XDG_RUNTIME_DIR=/run/user/0 HOME=/root ./build/lsearch -m --under /root year \
    # => rc=2 ; stderr: 搜索失败: running daemon does not support 'under' (no search2/count2); restart or upgrade lsearchd
# MCP 有 under → isError:true + 同一 kUnderUnsupportedMessage；无 under → 正常且 total_is_lower_bound=true
```
`Client::searchEx`/`countEx` 收到 `ERR unknown command` 时置 `v2_=0`；`under` 为空 → 回落 `search`，
`under` 非空 → 返回 false + `kUnderUnsupportedMessage`。MCP `supportsV2()` 经 `capabilities` 探测
（仅确定性结论缓存、瞬时故障下次重试），旧 daemon 走保留的 legacy over-fetch + cap refetch 分页，
legacy 页置 `total_is_lower_bound=true`（`total=0` 表示未知，而非"无命中"）。

### 自测回归（隔离 HOME/XDG_*，未触碰运行中的真实 daemon）
```
./scripts/self-test.sh -s       # 通过 25 项 / 失败 0 项（基线 22 → +3：--under 计数/搜索/失败退出码）
./scripts/self-test-ipc.sh -s   # 通过 18 项 / 失败 0 项（P1–P18，含旧 daemon stub 的 under 显式失败）
./scripts/self-test-mcp.sh -s   # 通过 37 项 / 失败 0 项（基线 29 → +8：S5b 诚实语义、S5c/S5d、S13a–f）
./scripts/self-test-dbus.sh -s  # 通过 22 项 / 失败 0 项（D-Bus 路径未回退）
```

### 性能证据（Spec 010 R6）
- 环境：Release 构建；`VACUUM INTO` 快照真实 SQLite 后由隔离守护进程加载（**317514 条**，非重扫）；
  CPU **16** 核；`iterations=25`（另 3 次预热）；每样本独立 Unix socket 连接。
- 命令：
```
python3 -c "import sqlite3; c=sqlite3.connect('file:/root/.local/share/lsearch/lsearch.db?mode=ro',uri=True); c.execute(\"VACUUM INTO '<ISOLATED>/data/lsearch/lsearch.db'\")"
HOME=<ISOLATED>/home XDG_DATA_HOME=<ISOLATED>/data XDG_CONFIG_HOME=<ISOLATED>/config \
  XDG_RUNTIME_DIR=<ISOLATED>/run build/lsearchd --foreground &
LSEARCH_PERF_SOCK=<ISOLATED>/run/lsearch.sock LSEARCH_PERF_PID=<pid> \
  python3 scripts/perf-search-v2.py 25
```
- 结果（Oracle 修复后重测；p50 / p95，ms）：

| 场景 | p50 | p95 | 门槛 | 结论 |
|---|---|---|---|---|
| 旧 `search` 宽查询 `a`（baseline） | 0.61 | 0.92 | < 10 | 通过 |
| `search2` 子串 宽 `a` | 19.36 | 21.62 | < 100 | 通过 |
| `search2` 子串 窄 `main.cpp` | 1.44 | 1.87 | < 30 | 通过 |
| `search2` + `under=/home/code/Lsearch` `lsearch` | 1.23 | 1.40 | < 30 | 通过 |
| `count2` 宽 `a` | 3.71 | 3.85 | < 30 | 通过 |
| `count2` 窄 `main.cpp` | 1.41 | 1.85 | < 30 | 通过 |
| 正则 宽 `re:.` | 18.50 | 19.35 | < 250 / 或排除 | 通过 |

- 峰值 RSS（宽 `search2` 后，`/proc/<pid>/status`）：`VmHWM=235348 kB (≈230 MiB)`；
  当前 `VmRSS=235348 kB (≈230 MiB)`。宽查询 >100ms 未出现，故无需给 `under` 收窄后的补充数值
  （仍附 `under` 场景一行）。

### 文档同步
`docs/specs/010-search-protocol.md`（本文件）、`docs/specs/README.md` 规格总览、`README.md`
（CLI `--under`、自测计数、新增 IPC 自测）、`docs/architecture.md`（检索语义 under/total/cap）、
`AGENTS.md` 步骤 3 计数、`docs/specs/001-index-core.md` 计数、Spec 006/007/008/009 计数与已知限制。
