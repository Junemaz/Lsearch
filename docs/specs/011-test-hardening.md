# Lsearch Spec 011 — 协议测试加固（外部验证器 + 真实转录回放 + 负形状矩阵）

Status: **Proposed**

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
- [ ] 官方 conformance 接入（stdio→HTTP 桥 + expected-failures 基线 + 摘要脚本）
- [ ] Inspector CLI 冒烟脚本
- [ ] wrapper + replay 脚本 + 合成 fixture（真实 fixture 待采集）
- [ ] 负形状矩阵脚本
- [ ] AGENTS.md 全局规则 + 006/010 交叉引用 + README/清单/计数同步

## Deliverable
`scripts/self-test-mcp-conformance.sh`、`scripts/self-test-mcp-inspector.sh`、
`scripts/mcp-trace-wrapper.sh`、`scripts/self-test-mcp-replay.sh`、
`scripts/self-test-mcp-matrix.sh`、conformance expected-failures 基线、fixture、文档

## Evidence
（未实现，无）
