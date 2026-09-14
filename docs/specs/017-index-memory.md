# Lsearch Spec 017 — 索引内存精简与可选低内存模式

Status: **Proposed**

## Why
真实索引（317,527 项）实测：

| 维度 | 实测 |
|---|---|
| DB 文件 | 77 MB |
| 原始字符串载荷（name+path） | **31 MB** |
| daemon RSS | **243 MiB** ≈ **800 B/项**（载荷的 ~8 倍） |
| 内存索引查询（含 CLI 启动 + IPC） | **7–12 ms**（纯扫描 ~1–3 ms） |
| 同一 DB 上 SQLite `LIKE '%q%'` | **25–48 ms/次**（子串无法走 B-tree，只能全表扫） |

结论：
1. **持久化不能替代内存**：内存买到的是子串匹配的毫秒级延迟；25–48 ms 对"即输即搜"是体验分水岭。
2. **但 243 MiB 不是必然代价**：真数据仅 31 MB，其余 ~212 MiB 来自每项两个 `std::string`
   （path/name）、`unordered_map` 路径索引与常驻排序数组——属**可消除的工程浪费**。
3. [Spec 016](016-watcher-correctness.md) 决定**默认索引隐藏文件**（条目数上升），内存问题更突出，
   故先精简结构、再把"是否常驻内存"变成显式取舍。

## What Changes
- **精简常驻索引**（目标 350k 项 ≤100 MiB，即 ~200–300 B/项）：
  - 单一 **arena**（连续字节池）+ 32/40-bit 偏移，替代每项 `std::string path`/`name`；
  - **去掉重复 name**（由 path 的 basename 偏移得到）；
  - **开放寻址哈希**（`path -> idx`）替代 `unordered_map<std::string, ...>`；
  - 排序结果**按需生成**，不常驻多份排序数组。
- **可选内存模式** `hot_index = memory | sqlite`（写进配置文件，`get-config` 可读、`set-opts` 可改）：
  - `memory`（**默认**）：低延迟路径（现状语义）；
  - `sqlite`：不常驻全量条目，查询走 DB（接受 25–48 ms/次量级），显著降内存；
  - 启动与模式切换各打印一行提示（延迟 vs 内存取舍），`stats` 暴露当前模式。
- **可测量**：`stats` 增加 `mode=`、`mem_est_bytes=`（常驻索引估算）与 `rss_bytes=`（可选）；
  新增 benchmark 脚本输出条目数 / RSS / P50 / P95（memory vs sqlite）。

## Requirements

#### Requirement 1 — 语义零回归
精简后搜索/排序/过滤/协议输出**逐字节不变**（唯一例外：`stats` 的追加字段）。
`search`/`search2`/`search3`/`count2` 的行为与错误串不变。

#### Requirement 2 — 内存显著下降且不劣化延迟
317k 项下 `hot_index=memory` 的 RSS **目标 ≤100 MiB**；查询延迟不劣化（P50 ≤ 现状 + 20%）。
以 benchmark 证据记录条目数、RSS、P50/P95。

#### Requirement 3 — `sqlite` 模式正确可用
`hot_index=sqlite` 的结果与 `memory` 模式**逐字节一致**（含 `total`/`total_capped`/排序/`under`）；
延迟有量化记录（量级 ~25–48 ms）；模式切换、重启、`--rebuild` 后的行为明确且一致。

#### Requirement 4 — 取舍可见
`stats.mode` 与内存估算可见；README/`--stats`/配置样例说明"内存换速度"的取舍与切换方法。

#### Requirement 5 — 不回归与文档
既有 12 套自测 + 单测 + conformance 全绿；`docs/architecture.md`（热索引/内存模型）、
README、规格清单同步（遵循文档同步全局规则）。

## 非目标
- 不在本规格引入 FTS5 trigram（作为后续候选，另立规格）。
- 不改协议版本（`stats` 新字段为向后兼容追加）。

## Scenario
Given 317k 项索引，`hot_index=memory`
When 查看 `stats` 并跑 benchmark
Then `mode=memory`，RSS ≤100 MiB（目标），P50 不劣于现状
Given 同一索引，`hot_index=sqlite`
When 跑同一查询语料
Then 结果与 memory 模式逐字节一致，RSS 明显下降，P50 记录在案
When 切换 `hot_index` 并重启
Then 行为与提示一致，无残留状态

## Task
- [ ] 设计并实现 arena/偏移布局（保持匹配与排序语义）
- [ ] 开放寻址 path 索引；去重 name
- [ ] `hot_index` 配置 + `stats`/`get-config`/`set-opts` 暴露 + 提示
- [ ] `sqlite` 查询模式（复用 `db_`；并发契约遵循 Spec 015）
- [ ] benchmark 脚本 + 证据（RSS / P50 / P95 / 条目数）
- [ ] 全量复跑 + 文档同步 + Evidence → Done

## Deliverable
`core/search.*`、`core/indexer.*`、`core/db.*`、`core/config.*`、`daemon/`、`scripts/`（benchmark）、文档

## Evidence
（待实现后填充）

### 立项数据（真实索引实测）
```
DB = 77 MB ; entries = 317527 ; name_bytes = 4.7 MB ; path_bytes = 26.2 MB ; payload = 30.9 MB
daemon RSS = 243.1 MiB  (~800 B/项)
SQLite LIKE %report% -> 48.2 ms ; %log% -> 25.5 ms ; %.md% -> 27.2 ms ; %zzz_absent% -> 24.7 ms
内存路径（lsearch --count，含启动+IPC）: report 12 ms ; log 8 ms ; zzz_absent 7 ms
```

## 依赖
与 [Spec 015](015-daemon-concurrency.md)（并发契约）与 [Spec 016](016-watcher-correctness.md)
（默认值变更）同触 `core/`+`daemon/`；整体顺序 **015 → 016 → 017**。
