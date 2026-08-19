# Lsearch Spec 000 — 项目背景与关键决策

## Why
Everything（Windows）之所以近瞬时，是直接读 NTFS 的 MFT + USN 变更日志——文件名索引
由文件系统免费维护。Linux/ext4 没有等价物，因此 Lsearch 必须**自建索引**：首次全量遍历 +
内存热索引 + SQLite 持久化 + inotify 增量更新。

## What Changes（目标）
为麒麟（Kylin V10 等信创桌面）提供 Everything 风格的文件名即时搜索，支持
CLI / TUI / GUI 多前端，共享同一份常驻索引。V1 交付 CLI+TUI，GUI 预留。

## Non-goals（V1 明确不做）
- 全文内容搜索（与 Everything 一致，仅文件名/路径）
- 守护进程离线期间改动的自动补齐（用 `--rebuild` 兜底）

## Key Decisions (ADR 简表)
| 决策点 | 结论 | 理由 |
|---|---|---|
| 运行模型 | 常驻守护进程 + 多前端 | 索引只建一次、inotify 只跑一份，前端零成本共享 |
| 索引持久化 | SQLite (WAL) | 重启免全量重扫，轻量可靠 |
| 检索内存 | 常驻内存热索引（预计算小写） | 查询进程内完成，极致速度 |
| 增量 | inotify（每个目录加 watch，新目录递归挂接） | Linux 标准实时文件事件 |
| IPC | Unix socket 明文行协议 | 简单、可 socat 调试；留 D-Bus 升级口 |
| 技术栈 | C++17 + SQLite + inotify（GUI 后续 Qt5） | 信创工具链成熟、UKUI 同源 |
| 默认索引范围 | 仅用户家目录 | 隐私与首扫速度，配置极简可扩展 |
| GUI 决策 | core/ipc 与前端完全解耦 | 新增 GUI 仅需新前端，复用 IPC client |

## Deliverable
- 可构建的三件套：`lsearchd` / `lsearch` / `lsearch-tui` + 单元测试 + 打包脚本
- 架构说明：[../architecture.md](../architecture.md)
