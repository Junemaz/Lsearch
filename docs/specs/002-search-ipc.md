# Lsearch Spec 002 — 检索语义 + IPC 协议

Status: **Done**

## Why
在自建索引之上提供"即输即搜"的检索能力，并让多前端（CLI/TUI/将来 GUI）通过统一
IPC 复用同一份索引。

## What Changes
- `core/search`：内存热索引，支持子串/通配符、排序、目录/文件过滤、候选上限
- `ipc\*`：Unix socket 明文协议 + client（连接失败自动拉起守护进程）

## Requirements

#### Requirement 1 — 检索语义（对齐 Everything 默认）
大小写不敏感子串匹配，命中"文件名或完整路径"；查询含 `*` 或 `?` 时切换为 glob 匹配
（`*` 任意序列、`?` 单字符，均大小写不敏感）。

#### Requirement 2 — 排序与过滤
支持按 name / path / size / mtime 排序（size、mtime 降序），支持仅目录 / 仅文件过滤。

#### Requirement 3 — 并发与上限
搜索与增量更新通过 `shared_mutex` 实现并发安全；宽泛查询受候选上限（5 万）约束并
上报截断标志，避免病态查询拖垮。

#### Requirement 4 — 空查询
空查询返回空结果（不返回全部）。

#### Requirement 5 — IPC 明文协议
`ping / version / stats / search <limit> <dirs> <files> <sort> <query> / rebuild /
add-path / remove-path / shutdown`；响应 `OK/ERR` + `END` 结构；socket chmod 0600。

#### Requirement 6 — 守护进程自动拉起
CLI/TUI 连不上 socket 时自动 `lsearchd`（默认开启，`-m` 关闭）。

#### Requirement 7 — 关闭安全
shutdown 时唤醒并回收所有连接线程，杜绝释放后使用（UAF）。

## Scenario: 即输即搜
Given 索引内存在 `AnnualReport.txt`、`vacation_photo.jpg`、`readme.md`、目录 `deeper`
When `lsearch report` / `lsearch '*.jpg'` / `lsearch --count a`
Then 分别命中对应条目；`--count a` 返回条目总数

## Task
- [x] Index.build/add/remove/removePathAndSubtree/collectDirs/search
- [x] 并行分块扫描 + 排序裁剪 + 候选上限
- [x] proto 文法 + Client（search/stats/command/auto-spawn）
- [x] daemon 连接线程跟踪 + shutdown 回收

## Deliverable
`core/{search,entry}.{h,cpp}`、`ipc/{proto,client}.{h,cpp}`、`daemon/daemon.cpp`

## Evidence
- 单测：`search_substring_case_insensitive`、`search_wildcard`、`search_sort_size`、
  `search_dirs_only`、`search_empty_query`、`search_limit`、`glob_match`（全部通过）
- 冒烟：`lsearch report` → `AnnualReport.txt`；`lsearch '*.jpg'` → 命中；
  `--count a` → 8；`--sort size -S deeper` → 按大小降序带详情
- 关闭安全回归：一连接保持打开的同时另一连接发送 `shutdown`，守护进程干净退出无崩溃
