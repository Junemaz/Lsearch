# Lsearch Spec 011 — 协议测试加固（外部验证器 + 真实转录回放 + 负形状矩阵）

Status: **Done**

## Why
两次真实事故都发生在"测试未覆盖的输入空间/降级路径"：
1. MCP 互操作：真实客户端（opencode）在 `tools/call` 携带 legacy `_meta.progressToken`，
   被当时自写 e2e driver 从未生成的形状触发误拒（`-32602`）。
2. Spec 010 评审发现：新客户端→旧 daemon 降级时非空 `under` 被静默丢弃（自写测试未覆盖该组合）。

根因是**测试 oracle 由实现方自写**（同源盲区）：自测只能证明"在我们假设的客户端下正确"，
不能证明"符合规范/真实客户端行为"。本规格为协议表面建立**外部验证**制度。

## What Changes
- **外部验证器接入**
  - 官方 conformance suite（`@modelcontextprotocol/conformance`，`server` 模式经 stdio→HTTP 桥
    `supergateway`/`mcp-proxy` 驱动 `lsearch-mcp`）；MCP 非目标场景（resources/sampling/
    elicitation/logging/completion 等）以 **expected-failures 基线**记录理由
  - MCP Inspector CLI（`@modelcontextprotocol/inspector --cli`）作为第三方客户端做
    tools/list + tools/call 冒烟
- **真实转录回放**：`scripts/mcp-trace-wrapper.sh` 记录真实客户端（opencode）双向 JSON-RPC 为
  JSONL；`scripts/self-test-mcp-replay.sh` 重放请求并断言响应（结构等价，忽略 id/时间）；
  至少保留 1 份真实会话转录作为 fixture
- **协议负形状矩阵**：表驱动用例覆盖 `_meta` 变体（缺失 / `progressToken` / 空对象 /
  仅 protocolVersion 缺 caps / 有效 modern / 非对象 / 未知键 + progressToken）、id 类型、
  通知、batch、方法大小写、参数类型
- **全局规则（AGENTS.md）**：协议面（MCP/IPC/D-Bus）的"完成"必须包含**至少一个外部验证器**
  （conformance / 真实客户端 / 第三方工具如 gdbus）；仅自写 driver 的自测不足以判定完成

## Requirements

#### Requirement 1 — 外部验证器可重复运行
提供脚本运行 conformance 与 Inspector CLI；环境不具备（无网络/无 npx）时以 **SKIP** 而非 FAIL
退出；输出目录与摘要可审计。

#### Requirement 2 — 真实转录回放
wrapper 以 JSONL 记录 `client→server` 与 `server→client` 行；replay 对每个请求断言响应
类型/关键字段（不做逐字节/时序断言）；至少包含一份真实 opencode 会话的转录 fixture
（采集需用户在真实会话中执行一次）。

#### Requirement 3 — 负形状矩阵
表驱动、无时序断言、断言错误码与 stdout 纯净（仅 JSON-RPC）；覆盖 Why 中列出的矩阵。

#### Requirement 4 — 不回归与文档
既有全部自测保持通过；新增脚本与 fixture 纳入 README/架构/规格清单与计数（遵循文档同步全局规则）。

#### Requirement 5 — 规则落地
`AGENTS.md` 增加"协议表面验证"全局规则；Spec 006/010 交叉引用本规格。

## 非目标
- 不实现 MCP resources/sampling/elicitation/日志能力（conformance 相应场景列入 expected-failures）
- 不引入托管 CI（本地/手动运行；后续 CI 落地时纳入）

## Scenario
Given 有网络与 npx
When 运行 `scripts/self-test-mcp-conformance.sh`
Then 官方 conformance `server` 套件对 `lsearch-mcp` 执行，结果与 expected-failures 基线比对，
      不适用场景有明确理由，脚本给出 PASS/FAIL 摘要
When 运行 `scripts/self-test-mcp-inspector.sh`
Then 第三方 Inspector CLI 能列出 2 个工具并成功调用 `index_stats`
When 运行 `scripts/self-test-mcp-replay.sh <fixture>`
Then 重放真实转录请求，响应与录制时的类型/关键字段一致
When 运行 `scripts/self-test-mcp-matrix.sh`
Then `_meta`/id/通知/batch/大小写各变体返回预期错误码（或成功），且 stdout 全为 JSON-RPC
When 环境无网络
Then 外部验证脚本 SKIP 并说明原因（不误报失败）

## Task
- [x] 官方 conformance 接入（stdio→HTTP 桥 + expected-failures 基线 + 摘要脚本）
- [x] Inspector CLI 冒烟脚本
- [x] wrapper + replay 脚本 + 合成 fixture（真实 fixture 待采集）
- [x] 负形状矩阵脚本
- [x] AGENTS.md 全局规则 + 006/010 交叉引用 + README/清单/计数同步

## Deliverable
`scripts/self-test-mcp-conformance.sh`、`scripts/self-test-mcp-inspector.sh`、
`scripts/mcp-trace-wrapper.sh`、`scripts/self-test-mcp-replay.sh`、
`scripts/self-test-mcp-matrix.sh`、conformance expected-failures 基线、fixture、文档

## Evidence

环境：node v22.22.1 / npx 9.2.0；`@modelcontextprotocol/conformance` **0.1.16**；
`@modelcontextprotocol/inspector`（CLI 模式）；`supergateway@3.4.3`（stdio→streamable HTTP 桥）。
本规格为 **TEST-ONLY**：未改动 `core/ mcp/ ipc/ daemon/ cli/ dbus/` 任何产品代码。

### R1 — 官方 conformance（stdio→HTTP 桥 + expected-failures 基线）
桥与套件（脚本内实际执行；仅给 npx 前缀加 `--prefer-offline`，优先用缓存、避免每次重新联网解析，
冷缓存仍回落网络）：
```
npx --prefer-offline -y supergateway@3.4.3 \
    --stdio "$PWD/build/lsearch-mcp" \
    --outputTransport streamableHttp --port <free-port> --streamableHttpPath /mcp
npx --prefer-offline -y @modelcontextprotocol/conformance server \
    --url "http://127.0.0.1:<port>/mcp" --suite active \
    --expected-failures scripts/mcp-conformance-baseline.yml -o <tmp>/conf-out
```
`./scripts/self-test-mcp-conformance.sh -s` → **PASS**（退出码 0）：
```
Total: 4 passed, 26 failed
✗ dns-rebinding-protection: 1 passed, 1 failed
Baseline check passed: all failures are expected.
...
  MCP conformance：PASS（所有失败均为基线内预期）✅
```
- suite=active 共 **30** 个场景；基线 `scripts/mcp-conformance-baseline.yml` 收录 **27** 条
  预期失败（26 条 FAILURE + 1 条 WARNING `server-sse-multiple-streams`），每条附一行理由：
  能力非目标（logging/completion/elicitation/resources/prompts/sampling）、
  参考 everything-server 工具面（`tools-call-*` 调用 `test_simple_text` 等本服务不暴露的工具）、
  HTTP 桥职责（DNS 重绑定防护、SSE 会话）——均见 Spec 006「非目标」。
- 范围内 **3** 个场景实测通过且**不**在基线：`server-initialize`、`ping`、`tools-list`
  （一旦回归即退出码 1，不会被基线掩盖）。
- 失败时保留 `-o` 输出目录与桥日志；成功时清理。SKIP 不产生假 FAIL。

### R1（续）— Inspector CLI 第三方客户端冒烟
`./scripts/self-test-mcp-inspector.sh -s` → **通过 3 项 / 失败 0 项**：
```
PASS  inspector tools/list exact 2 tools tools=['index_stats', 'search_files']
PASS  inspector tools/call index_stats isError=false, text content
PASS  inspector index_stats payload files=3
```
命令：`npx --prefer-offline -y @modelcontextprotocol/inspector --cli build/lsearch-mcp
--method tools/list --format json` 与 `--method tools/call --tool-name index_stats
--tool-args-json '{}' --format json`；隔离 HOME/XDG 下 index_stats `roots` 命中隔离 HOME。

### R2 — 真实转录 wrapper + replay
- `scripts/mcp-trace-wrapper.sh`：透明 stdio 代理，双向追加 JSONL
  `{"dir":"c2s"|"s2c","line":"<原始行>"}` 后转发给 `build/lsearch-mcp`（不解析、不改写载荷）。
- `tests/fixtures/mcp-trace-synthetic.jsonl`：由 wrapper 对隔离 daemon **实跑生成**，9 行
  （5 c2s + 4 s2c；legacy `initialize` → `notifications/initialized` → `tools/list` →
  `tools/call index_stats` → `tools/call search_files{report}` → stdin EOF 生命周期）。
- `./scripts/self-test-mcp-replay.sh -s` → **通过 6 项 / 失败 0 项**（结构等价：标量归一为类型、
  列表不比长度、忽略 id/时间；不做逐字节/时序断言）：
```
PASS  replay[0] initialize id=1
PASS  replay[2] tools/list id=2
PASS  replay[3] tools/call id=3 index_stats
PASS  replay[4] tools/call id=4 search_files
PASS  replay lifecycle stdin-EOF exit 0
PASS  replay stdout all JSON-RPC
```
### R2（续）— 真实 opencode 转录（已采集并长期保存）
- `tests/fixtures/mcp-trace-real.jsonl`：真实 opencode 经 wrapper 的一次会话（11 行 = 7 c2s + 4 s2c）。
  采集方式：把 `~/.config/opencode/opencode.jsonc` 的 lsearch MCP `command` 临时指向
  `scripts/mcp-trace-wrapper.sh`，`environment` 设 `LSEARCH_MCP_TRACE` 与
  `LSEARCH_MCP_BIN=build/lsearch-mcp`（真实客户端字节 + 当前实现），重启 opencode 后各调一次
  `index_stats` / `search_files`；采集后配置已还原。
- 真实客户端行为（合成 fixture 无法保证，现已被此 fixture 锁定）：
  - `initialize` **不带** `"jsonrpc"` 字段（与合成 fixture 不同）；
  - 两个 `tools/call` 都带 **legacy** `_meta:{"progressToken":<number>}`——即最初触发 `-32602` 的形态；
  - 会话结束发 `notifications/cancelled{requestId,reason:"AbortError…"}`。
- `./scripts/self-test-mcp-replay.sh -s tests/fixtures/mcp-trace-real.jsonl` → **通过 6 项 / 失败 0 项**。
- 驱动放宽（结构等价语义，另见偏差记录）：空数组与任意数组视为**结构兼容**——回放跑在隔离空索引上，
  结果集基数（`search_files.results` 条数）由数据决定，不构成协议差异；非空时仍逐元素比结构。

### R3 — 负形状矩阵（表驱动、无 timing 断言）
`./scripts/self-test-mcp-matrix.sh -s` → **通过 27 项 / 失败 0 项**，覆盖：
`_meta` 变体（缺失 / 仅 `progressToken` / `{}` / 仅 `protocolVersion` 缺 `clientCapabilities`
→ -32602 / 有效 modern / 非对象 / 未知键+`progressToken`）、id 类型（int/string/float/large
原样回显；`null` 按通知处理无响应）、通知（`notifications/initialized`、`notifications/cancelled`
无响应）、JSON 数组批次 → -32600、非对象请求 → -32600、缺 `method` → -32600、
`Tools/List` → -32601、`tools/call`（params 非对象 / 缺 `arguments` / `query`·`limit`·`under`
类型错误 / 未知工具 / `index_stats` 带非空参数）→ -32602；stdout 全为合法 JSON-RPC、EOF 退出 0。

### SKIP 行为（无 npx / 无网络，exit 0 不误报 FAIL）
```
$ PATH=/tmp/opencode/skipbin ./scripts/self-test-mcp-conformance.sh -s
SKIP: 未找到 npx                                        # exit 0
$ PATH=/tmp/opencode/skipbin ./scripts/self-test-mcp-inspector.sh -s
SKIP: 未找到 npx                                        # exit 0
# npx 存在但解析失败（模拟断网）：
SKIP: 无法解析 @modelcontextprotocol/conformance（npx 缓存缺失且无网络）   # exit 0
SKIP: 无法解析 @modelcontextprotocol/inspector（npx 缓存缺失且无网络）     # exit 0
```

### 回归（未改产品代码；全部保持绿色）
```
./build/lsearch_tests            # 481 checks / 0 failures
./scripts/self-test.sh -s        # 通过 25 项 / 失败 0 项
./scripts/self-test-ipc.sh -s    # 通过 18 项 / 失败 0 项
./scripts/self-test-mcp.sh -s    # 通过 37 项 / 失败 0 项
./scripts/self-test-dbus.sh -s   # 通过 22 项 / 失败 0 项
```

### 真实服务器缺陷
本次外部验证（官方 conformance + 第三方 Inspector）**未发现** `lsearch-mcp` 产品缺陷；
范围内 `server-initialize` / `ping` / `tools-list` 均通过，基线外无新增失败。
说明：矩阵为自写测试（非外部验证器）；其中 `id:null` 被按通知处理（无响应）——MCP `RequestId`
为 `string | number`，显式 `null` 不属合法 MCP 请求，故按既定实现断言并在此记录，
不作为服务器缺陷（遵循本规格 TEST-ONLY 约束）。

### 偏差记录
- 桥选用 **supergateway@3.4.3**（`--outputTransport streamableHttp`）；mcp-proxy 未采用
  （supergateway 一次成功且为任务首选）。
- npx 统一加 `--prefer-offline`（仅此一处偏离任务给出的原命令；语义等价，只影响解析缓存策略）。
- 不引入托管 CI（本规格非目标）。
- replay 结构等价放宽：**空数组 ↔ 任意数组视为兼容**（数据依赖的结果集基数不比长度），
  否则录于真实索引的转录无法在隔离空环境回放；详见 R2（续）。

### 独立复验发现的测试自身缺陷（已修复）
实现完成后的独立复验中，运行 `self-test-mcp-inspector.sh` 暴露了**测试自身**的泄漏与脆弱性
（非产品缺陷），均已修复：

1. **隔离 daemon 泄漏（真问题）**：第三方 Inspector 以**精简环境**启动 `lsearch-mcp`（仅 `HOME`，
   无 `XDG_*`），其自动拉起的 `lsearchd` 落到回退路径（socket `/tmp/lsearch-$UID`、数据
   `$HOME/.local/share`）；脚本 cleanup 只关停"自己环境"下的实例 → 每跑一次泄漏一个 daemon。
   修复：新增共享库 `scripts/selftest-daemon-guard.sh`（启动前快照既有 `lsearchd`，cleanup 时只
   回收运行期间新增实例、绝不触碰既存 daemon），接入 conformance/inspector/matrix/replay 四脚本。
   验证：四脚本运行前后 `pgrep -x lsearchd` 集合完全不变。
2. **Inspector 调用脆弱**：每次调用都经 `npx` 重新解析包，离线/代理抖动时偶发
   `sh: 1: mcp-inspector: not found`（曾连续 PASS 两次后失败一次）。修复：启动时一次性解析
   `mcp-inspector` 绝对路径并直接调用；解析失败走 SKIP（不误报 FAIL）。验证：连续 3 次 3/3。
3. **产品侧观察（非缺陷，记录备查）**：第三方宿主若剥离 `XDG_*`，daemon 会落到
   `/tmp/lsearch-$UID` + `$HOME/.local/share`；宿主集成应显式传递
   `XDG_RUNTIME_DIR`/`XDG_DATA_HOME`，否则索引位置与用户预期不一致。
4. `.gitignore` 增加 `.selftest*/` 与 `.tmp/`（覆盖测试临时目录变体）。

