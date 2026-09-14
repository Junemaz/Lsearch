# Lsearch 规格清单（OpenSpec 风格）

本目录把 Lsearch 的开发组织成「规格 → 实现 → 验证」的可核对单元。
每条规格自带 `Requirement`（可验收的需求）与 `Evidence`（证明其成立的测试/命令），
与仓内实现一一对应，无需任何外部工具即可审计。

## 工作流约定
1. 新需求先写成 `specs/NNN-*.md`（状态 Proposed）。
2. 开始实现 → 状态 In Progress。
3. 实现完，跑对应单测与冒烟，把结果写进该规格的 `Evidence` → 状态 **Done**。
4. **提交前必须同步所有受影响文档**（本清单、受影响规格的 Evidence、README/architecture 的
   计数与行为描述等）；文档滞后于实现视为未完成，不得提交。
5. 协议面（MCP/IPC/D-Bus）另需**外部验证器**（conformance / 真实客户端 / 第三方工具）作为
   完成条件（见 [011-test-hardening](011-test-hardening.md)）。

## 状态总览

| 规格 | 内容 | 状态 | 关键证据 |
|---|---|---|---|
| [000-project](000-project.md) | 项目背景、目标、关键决策(ADR) | Done | — |
| [001-index-core](001-index-core.md) | 索引核心：遍历 / DB / inotify 增量 | **Done** | `lsearch_tests`、冒烟 |
| [002-search-ipc](002-search-ipc.md) | 检索语义 + IPC 协议 | **Done** | `lsearch_tests`、冒烟 |
| [003-cli-tui](003-cli-tui.md) | CLI 与 TUI 前端 | **Done** | 端到端冒烟 |
| [004-packaging](004-packaging.md) | deb/rpm 打包、多架构 | **Done**(deb/rpm 构建实测) / 多架构待验证 | `packaging/*.deb`、`*.rpm` |
| [005-gui-v2](005-gui-v2.md) | V2：Qt5 GUI | **Done**（麒麟实机安装待验证） | offscreen 冒烟、WSLg 验证、deb/rpm 含 GUI |
| [006-lsearch-mcp](006-lsearch-mcp.md) | MCP 前端（stdio，LLM 编码代理） | **Done** | `lsearch_tests` 481、`self-test-mcp.sh` 37/37 |
| [007-daemon-singleton](007-daemon-singleton.md) | 守护进程单例锁（并发启动竞态） | **Done** | `scripts/self-test.sh` 25/25（含单例 4 项） |
| [008-regex](008-regex.md) | V3：正则匹配（`re:` 前缀，ECMAScript） | **Done** | `lsearch_tests` 481、`self-test.sh` 25/25、`self-test-mcp.sh` 37/37 |
| [009-dbus](009-dbus.md) | V3：D-Bus 集成（桌面门面 + 按需激活） | **Done** | `lsearch_tests` 481、`self-test-dbus.sh` 22/22 |
| [010-search-protocol](010-search-protocol.md) | 检索协议 v2：真 total/truncated + 路径子树过滤 | **Done** | `lsearch_tests` 481、`self-test-ipc.sh` 18/18、性能证据 |
| [011-test-hardening](011-test-hardening.md) | 协议测试加固（外部验证器 + 转录回放 + 负形状矩阵） | **Done** | conformance `server` PASS（基线内预期）、Inspector 3/3、replay 6/6（合成 + 真实 opencode 转录）、matrix 27/27 |
| [012-ipc-hardening](012-ipc-hardening.md) | IPC 协议加固（外部验证 + 帧完整性修复） | **Done** | `nc -U` 第三方 7/7、独立客户端×CLI 逐字节差分 13/13、负形状矩阵 33/33；`search3` 转义修复 `\n`/`\t` 帧破坏；`lsearch_tests` 511 |
| [013-ipc-input-validation](013-ipc-input-validation.md) | IPC 输入校验与配置往返完整性（`ERR bad path`/`ERR bad limit`） | **Done** | `lsearch_tests` 552、IPC 矩阵 61/61（含拒绝后零副作用与 `remove-path` 落盘）、外部 7/7、差分 13/13 |
| [015-daemon-concurrency](015-daemon-concurrency.md) | 守护进程并发安全（重建 × 管理命令竞态） | **Done** | 根因=detached 重建线程并发改 watcher/sqlite/cfg；修复后我方复现 20 轮 add/remove **0 崩溃**、矩阵 61/61×3、全套件绿 |
| [016-watcher-correctness](016-watcher-correctness.md) | watcher 增量正确性（隐藏过滤 / 重命名 / 统计溢出） | **Done** | 修 4 类缺陷（含"watcher 重启后零 watch 致增量静默失效"与"索引根从未被 watch"）；我方探针 11/11、矩阵 78/78、单测 574 |
| [017-index-memory](017-index-memory.md) | 索引内存精简与可选低内存模式（`hot_index=memory\\|sqlite`） | **In Progress** | 基线（bench-search.sh）：317k 项 RSS 194 MiB（≈642 B/项）、P50 3.0–3.7 ms；目标 ≤100 MiB |

如何核对：每张规格的 `## Scenario` 是"用户可复现的验收场景"，`## Evidence` 给出
"我如何证明它成立"（单测名 + 命令）。
