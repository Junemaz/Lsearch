# Lsearch Spec 003 — CLI 与 TUI 前端

Status: **Done**

## Why
提供面向终端的两类前端：CLI 面向脚本与批量，TUI 面向交互搜索（Everything 的"输入即搜"
体验在纯终端复现）。

## What Changes
- `cli/lsearch`：搜索、通配符、排序、计数、详情、NUL 输出、自动拉起守护进程
- `tui/lsearch-tui`：ncurses 交互界面

## Requirements

#### Requirement 1 — CLI 基础
`lsearch [选项] <关键词>` 输出命中路径；支持 `-l/--limit`、`-s/--sort`、`-c/--count`、
`-d/--dirs-only`、`-f/--files-only`、`-S/--details`、`-0/--print0`、`-m`（不自动拉起）、
`--stats`、`--rebuild`、`-v`、`-h`。

#### Requirement 2 — CLI 退出码（脚本友好）
命中>0 → 0；无命中 → 1；用法/连接错误 → 2。

#### Requirement 3 — CLI `--print0`
以 NUL 分隔输出路径，且**末尾不得追加换行**，保证 `xargs -0` 可用。

#### Requirement 4 — TUI 交互
顶部搜索框（UTF-8 输入即搜，宽字符支持）、中段可滚动结果列表（命中子串高亮、目录标记）、
底部状态栏（索引统计 + 匹配数 + 快捷键提示）。操作：↑↓/PgUp/PgDn/Home/End 选择，
Enter 用 `xdg-open` 打开，F5 重建索引，Esc 清空，Ctrl+C/Ctrl+Q 退出，Ctrl+L 重绘。

## Scenario: 终端搜索与打开
Given 守护进程运行
When 在 TUI 输入 `report` → 结果显示 `AnnualReport.txt`（子串高亮）
When Enter → `xdg-open` 被调用打开该文件
When F5 → 状态栏提示"已触发重建…"
When 输入空 → 清空结果；Ctrl+Q → 退出

## Task
- [x] cli 参数解析 + 输出 + 退出码
- [x] tui 三窗格 + 输入即搜（后台线程防抖）+ xdg-open + 快捷键
- [x] 两端复用 Client；连接失败自动拉起 lsearchd

## Deliverable
`cli/main.cpp`、`tui/main.cpp`

## Evidence
- 冒烟（同一命令内起守护进程实测）：`lsearch report`、`lsearch '*.jpg'`、
  `lsearch -m --count a`、`lsearch -m --sort size -S deeper`、`lsearch -m -d a` 全部符合预期
- TUI 伪终端冒烟：能启动、连上守护进程、渲染搜索框与状态栏（`files=.. dirs=.. size=..`）
  —— 真实交互需在实体终端完成
- `-0` 修复后经代码审查确认末尾无多余 `\n`
