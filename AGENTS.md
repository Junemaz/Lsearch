# AGENTS.md — 给在此仓库工作的 Agent 的指引

## 项目
Lsearch：面向麒麟桌面的 Everything 风格文件名搜索。
架构 = 常驻守护进程 `lsearchd`（SQLite + 内存索引 + inotify）+ 多前端
（CLI `lsearch` / TUI `lsearch-tui` / 规划中的 Qt GUI），通过 Unix socket 明文协议通信。
详见 [docs/architecture.md](docs/architecture.md) 与 [README.md](README.md)。

## 规格驱动（必读）
本仓库采用规格驱动工作流，所有开发应由规格驱动、并以测试证明：

1. 开工前阅读 [docs/specs/README.md](docs/specs/README.md)，看相关 `docs/specs/NNN-*.md`。
2. 新需求先写一张规格（Proposed），再实现；实现后把单测/冒烟结果填入该规格的
   `## Evidence`，并将状态改为 Done。
3. 单测入口：`cmake --build build && ./build/lsearch_tests`（当前 481 checks / 0 failures）。
4. 端到端：守护进程与 CLI 必须在**同一条 shell 命令**里运行（本沙箱 /tmp 是每条命令
   独立的 tmpfs，后台进程随容器回收）；用隔离的 `HOME/XDG_*` 指向工作区下临时目录。

## 改动约定
- `core/` 不依赖任何 UI，GUI 相关只允许新增前端、复用 `ipc/client`。
- C++17；默认仅索引用户家目录；配置保持纯文本极简。
- 关键不变量：守护进程 shutdown 必须回收全部连接线程（不得 detach 而越过 Daemon 生命周期）。
- 提交信息用 `<type>(<scope>): <中文说明>`，可附收尾的修复点清单。
- **文档同步（全局规则）**：每次功能修改，提交前必须更新所有受影响文档（README、
  docs/architecture.md、docs/specs/* 的状态/Evidence/清单、各处计数等）。任何文档滞后于
  实现，视为功能未完成，不得提交。
- **协议表面验证（全局规则）**：MCP / IPC / D-Bus 等协议面的功能，"完成"的定义必须包含
  **至少一个外部验证器**（官方 conformance 套件、真实客户端、或第三方工具如 gdbus）；
  仅自写 driver 的自测不足以判定完成（见 Spec 011）。

## 参考
开源的 Fsearch / fd / fzf / plocate 用于借鉴索引更新与遍历技巧，不直接整段照搬。
