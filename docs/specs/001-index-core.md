# Lsearch Spec 001 — 索引核心（遍历 / DB / inotify 增量）

Status: **Done**

## Why
Everything 的"免费索引"在 Linux 不存在，必须自建。本规格定义索引的三件基础能力：
全量建立、持久化、实时增量，其余系统均依赖它们。

## What Changes
- `core/indexer`：并行目录遍历，产出 `FileEntry`（路径/名/大小/mtime/是否目录/inode）
- `core/db`：SQLite 持久化（`files` + `meta` 表，WAL 模式）
- `core/watcher`：inotify 实时增量（增/删/改，新目录递归挂接，watch 上限告警）

## Requirements

#### Requirement 1 — 并行全量遍历
遍历配置的根路径，默认跳过系统伪目录（/proc /sys /dev /run）与排除前缀、隐藏文件；
支持不跟随符号链接（默认），目录数量统计正确，遍历过程可上报进度。

#### Requirement 2 — SQLite 持久化
`files(path PK, name, size, mtime, is_dir, inode)` + `meta(key,value)`；WAL 模式；
支持单条 upsert、按路径删除、`removeSubtree`（路径 + 其全部后代）、全量载入、清空、meta 读写。

#### Requirement 3 — 目录创建/删除的正确性
删除一个目录时，DB 与内存索引中该目录及其全部后代一起删除（`removeSubtree` 语义）。

#### Requirement 4 — inotify 实时增量
被索引目录都能收到创建/删除/移动/修改事件；新出现的目录自动加 watch 并扫描其内容；
watch 数量触顶（ENOSPC）时打印一次告警而不崩溃。

#### Requirement 5 — 守护进程重启快速恢复
重启优先从 SQLite 加载索引（免重扫）；DB 为空或强制重建时全量遍历。

## Scenario: 全量 → 增量
Given 一个含若干文件/子目录的根路径
When `lsearchd` 首次启动建索引
Then 搜索能命中全部条目，`stats` 给出的 files/dirs 与磁盘一致（单测 + 冒烟）

Given 守护进程运行中
When 新建文件 `BrandNewDoc.pdf`
Then 约 1 秒内 `lsearch BrandNewDoc` 即可命中
When `rm BrandNewDoc.pdf`
Then `lsearch BrandNewDoc` 回到无结果（退出码 1）

## Task
- [x] indexer 并行遍历 + 排除/隐藏规则
- [x] db schema + 事务批量 upsert + loadAll + removeSubtree
- [x] watcher inotify 事件分发 + 目录递归 + ENOSPC 告警
- [x] daemon init 从 DB 恢复 + `LSEARCH_FORCE_RESCAN` 强制重建

## Deliverable
`core/{indexer,db,watcher,config,util}.{h,cpp}` + `daemon/daemon.{h,cpp}`

## Evidence
- 单测（`./build/lsearch_tests`，55 checks / 0 failures）：
  `index_add_remove`、`index_remove_subtree`、`db_roundtrip`、`config_*`
- 冒烟（隔离 HOME 实测）：
  - 建索引：`[lsearchd] scan done: 8 entries`；`stats` → files=8 dirs=4
  - 增量：新建 `BrandNewDoc.pdf` → 立即命中；删除 → 立即消失（退出码 1）
  - `--rebuild` 空启动：daemon 停机期间新增文件，重启 `--rebuild` 后命中
