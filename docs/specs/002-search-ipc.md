# Lsearch Spec 002 — 检索语义 + IPC 协议

Status: **Done**

## Why
在自建索引之上提供"即输即搜"的检索能力，并让多前端（CLI/TUI/将来 GUI）通过统一
IPC 复用同一份索引。

## What Changes
- `core/search`：内存热索引，支持子串/通配符、排序、目录/文件过滤、候选上限
- `ipc\*`：Unix socket 明文协议 + client（连接失败自动拉起守护进程）

## Requirements

#### Requirement 1 — 检索语义（仅按名称匹配）
大小写不敏感子串匹配，仅命中**最终文件/文件夹名（basename）**——完整路径不参与匹配
（避免搜 `code` 时整批 /home/code/* 因路径命中而刷屏）；查询含 `*` 或 `?` 时切换为
glob 匹配（`*` 任意序列、`?` 单字符，均大小写不敏感）。

#### Requirement 2 — 排序与过滤
支持按 name / path / size / mtime 排序（size、mtime 降序），支持仅目录 / 仅文件过滤。

#### Requirement 3 — 并发与上限
搜索与增量更新通过 `shared_mutex` 实现并发安全；宽泛查询受候选上限（5 万）约束并
上报截断标志，避免病态查询拖垮。

#### Requirement 4 — 空查询
空查询返回空结果（不返回全部）。

#### Requirement 5 — IPC 明文协议
命令全集：`ping / version / stats / search / search2 / search3 / count2 / capabilities /
rebuild / add-path / remove-path / shutdown / get-config / set-paths / set-excludes /
set-opts`。**并非所有响应都带 `END`**：
- 多行响应（`OK` 头 + 负载 + `END`）：`version`、`stats`、`search`、`search2`、`search3`、
  `capabilities`、`get-config`；
- 单行响应（仅 `OK` / `ERR`，**无 `END`**）：`ping`、`count2`、`rebuild`、`add-path`、
  `remove-path`、`shutdown`、`set-paths`、`set-excludes`、`set-opts`。

socket chmod 0600，socket 目录 `mkdir 0700`，单例锁文件 `<sock>.lock`（flock，0600，
随进程退出释放）。单行请求上限 1 MiB，超限返回 `ERR line too long` 并关闭该连接。

路径与 `limit` 校验（[Spec 013](013-ipc-input-validation.md)）：
- `add-path` 的参数**不做 CSV 分割**（整串即一个路径）；`set-paths` / `set-excludes`
  才按逗号取 CSV 并对**每个元素**校验。合法路径 = 非空、不含 `,`、不含控制字符
  （`<0x20` 与 `0x7f`）、无首尾空格/TAB、长度 ≤ 4096；违规返回 `ERR bad path`，
  且不修改内存配置、不落盘、不触发重建。`set-paths` 整体为空仍是
  `ERR paths must not be empty`；`set-excludes` 的 `_` 哨兵仅整体参数等于 `_` 时清空。
- `remove-path` 仅校验非空（便于精确移除历史上含逗号的条目），成功移除后**调用
  `Config::save()` 落盘**（守护进程重启后不再复活）。
- `search` / `search2` / `search3` 的 `limit` 必须为纯十进制 ASCII 数字（非空、无符号、
  无前导 `+`）且 `0 ≤ limit ≤ 1048576`，否则返回 `ERR bad limit`；校验顺序为
  `splitHead` → `limit` → `sort` → `under` → `query`。合法输入响应逐字节不变（`0` 仍表示不限制）。

（追加式扩展见 [Spec 010](010-search-protocol.md)：`search2`/`count2`/`capabilities`；
帧完整性 `search3` + 行上限见 [Spec 012](012-ipc-hardening.md)。）

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
