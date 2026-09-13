# Lsearch 规格清单（OpenSpec 风格）

本目录把 Lsearch 的开发组织成「规格 → 实现 → 验证」的可核对单元。
每条规格自带 `Requirement`（可验收的需求）与 `Evidence`（证明其成立的测试/命令），
与仓内实现一一对应，无需任何外部工具即可审计。

## 工作流约定
1. 新需求先写成 `specs/NNN-*.md`（状态 Proposed）。
2. 开始实现 → 状态 In Progress。
3. 实现完，跑对应单测与冒烟，把结果写进该规格的 `Evidence` → 状态 **Done**。

## 状态总览

| 规格 | 内容 | 状态 | 关键证据 |
|---|---|---|---|
| [000-project](000-project.md) | 项目背景、目标、关键决策(ADR) | Done | — |
| [001-index-core](001-index-core.md) | 索引核心：遍历 / DB / inotify 增量 | **Done** | `lsearch_tests`、冒烟 |
| [002-search-ipc](002-search-ipc.md) | 检索语义 + IPC 协议 | **Done** | `lsearch_tests`、冒烟 |
| [003-cli-tui](003-cli-tui.md) | CLI 与 TUI 前端 | **Done** | 端到端冒烟 |
| [004-packaging](004-packaging.md) | deb/rpm 打包、多架构 | **Done**(deb) / 待验证(rpm) | `packaging/*.deb` |
| [005-gui-v2](005-gui-v2.md) | V2：Qt5 GUI（规划） | **Proposed** | — |
| [006-lsearch-mcp](006-lsearch-mcp.md) | MCP 前端（stdio，LLM 编码代理） | **Done** | `lsearch_tests` 263、`self-test-mcp.sh` 28/28 |
| [007-daemon-singleton](007-daemon-singleton.md) | 守护进程单例锁（并发启动竞态） | **Done** | `scripts/self-test.sh` 22/22（含单例 4 项） |
| [008-regex](008-regex.md) | V3：正则匹配（`re:` 前缀，ECMAScript） | **Done** | `lsearch_tests` 263、`self-test.sh` 22/22、`self-test-mcp.sh` 28/28 |

如何核对：每张规格的 `## Scenario` 是"用户可复现的验收场景"，`## Evidence` 给出
"我如何证明它成立"（单测名 + 命令）。
