# Lsearch Spec 016 — watcher 增量正确性（隐藏过滤 / 重命名 / 统计溢出）

Status: **Proposed**

## Why
用户报告的"隐藏文件增量不一致"经隔离环境复现，牵出**四个真实缺陷**（`index_hidden=0` 默认下实测）：

```
初始 count(hidprobe) = 0
$HOME 新建 .hidprobe1.md          -> count=0   （正确：事件被合并进 IN_CREATE 分支，有隐藏过滤）
$HOME/sub 新建 .hidmv3.md         -> count=1   （错误：走 IN_MODIFY/IN_ATTRIB 分支，该分支无隐藏过滤）
可见文件 temp -> rename vis2.md   -> count=0   （错误：mv/编辑器原子保存的新名未进索引）
隐藏目录内 inside.md              -> count=0   （表象正确）
```

- **F1（事件分支不一致）**：`core/watcher.cpp` 的 `IN_CREATE|IN_MOVED_TO` 分支检查隐藏
  （`loop()` 119–120 行），而 `IN_MODIFY|IN_ATTRIB`（"仅对现存条目做更新"）分支**完全不检查**
  （124–137 行）。inotify 是否把 create+modify 合并为同一事件，决定走哪条分支 → **同一操作有时
  被过滤、有时被索引**，这就是用户观察到的"不一致"。
- **F2（重命名/原子保存丢失）**：`mv`（temp → 新名，编辑器保存的常见模式）后新名未进索引
  （可见文件实测 count=0）。`IN_MOVED_TO` 虽在分支内，但实测无效；需在实现期用事件掩码观测
  定位（合并掩码的处理顺序、`IN_MOVED_FROM` 的 `else if` 吞噬等）。
- **F3（统计溢出）**：`stats.size` 报出 `8388608.0 TB`（= 2^63 bytes）——`Index::totalBytes()`
  累加溢出，非真实值。
- **F4（条件写反）**：`watcher.cpp:155` 为 `if (cfg_.index_hidden && isHiddenName(baseName(d)))
  continue;` —— 恰好在 `index_hidden=1` 时**跳过隐藏目录**的 watch，与"索引隐藏文件"语义相反；
  在用户希望"把隐藏文件纳入索引"的目标下必须修正。

## What Changes
- 把"该名字是否索引"（`index_hidden` + `excludes`）收敛为**单一判定函数**，事件路径
  （`IN_CREATE`/`IN_MOVED_TO`/`IN_MODIFY`/`IN_ATTRIB`/`scanNewDir`/`start`）全部复用，消除分支差异。
- 修复重命名/原子保存语义：`IN_MOVED_TO` 必须落索引；`IN_MOVED_FROM` 仅在目标确实消失时删除；
  明确同一掩码多位同时置位时的处理顺序（先删旧、后加新或等价保证）。
- 修复 `Index::totalBytes()` 溢出（`uint64_t` 累加 + 大值单测）。
- 修正 `Watcher::start()` 的隐藏目录条件，使其与 `index_hidden` 语义一致。
- 回归测试：覆盖隐藏文件的各事件路径、`mv`/原子保存（同目录 + 跨目录）、隐藏目录、
  以及 >2^31 字节的 `size` 累加。

## Requirements

#### Requirement 1 — 隐藏过滤在所有事件路径一致（含默认值变更）
`index_hidden=0`：任何事件路径（create/modify/attrib/moved_to/子目录扫描/启动）都不得把隐藏
文件或隐藏目录放入索引。`index_hidden=1`：必须索引隐藏文件与隐藏目录。

**默认值变更（已定）**：`index_hidden` 默认改为 **1**（索引隐藏文件与隐藏目录），并默认排除少量
高频噪音：`.git`、`.cache`、`~/.local/share/Trash`（写入 `Config::defaults()` 的 excludes，
并在 README/`--stats` 说明）。因此 R1 的两个方向都必须有回归覆盖；`=0` 仍是受支持的可选行为。

#### Requirement 2 — 重命名与原子保存
`mv`（同目录与跨目录、temp→可见名、temp→隐藏名）之后：新名按 R1 规则出现在索引中，
旧名不残留（除非仍存在）。

#### Requirement 3 — 统计正确
`stats.size` 为所有条目 `size` 的真实和（`uint64_t`，无溢出）；有单测覆盖大值累加。

#### Requirement 4 — 回归与不变量
既有 12 套自测 + 单测 + conformance 全绿；新增用例在修复前**可稳定失败**、修复后通过。

#### Requirement 5 — 文档同步
`docs/architecture.md` 的 inotify/增量说明与 Spec 001 交叉引用；README/规格清单同步。

## 非目标
- 不改协议与命令语义；不引入 fanotify；不处理 inotify 队列溢出丢事件（维持既有告警）。

## Scenario
Given 隔离 daemon（`index_hidden=0`）
When 在任意（含子）目录新建/修改隐藏文件，或用 `mv` 生成隐藏名
Then 该隐藏文件始终不在索引中
When 用 `mv`（temp → 可见名）落盘可见文件
Then 新名出现在索引中，旧名不残留
Given `index_hidden=1`
When 新建隐藏文件与隐藏目录
Then 二者都被索引
When 查询 `stats.size`
Then 为真实字节和（不出现 2^63 量级）

## Task
- [ ] 统一"是否索引该名字"判定并接入全部事件路径
- [ ] 修复 `IN_MOVED_TO`/`IN_MOVED_FROM` 语义（含事件掩码顺序）
- [ ] 修复 `totalBytes()` 溢出（+ 单测）
- [ ] 修正 `Watcher::start()` 隐藏条件
- [ ] `index_hidden` 默认改 1 + 默认排除 `.git`/`.cache`/`~/.local/share/Trash`（文档同步）
- [ ] 回归用例（隐藏各路径 / mv 原子保存 / 大 size / 默认值两方向）
- [ ] 全量复跑；文档同步；Evidence → Done

## 依赖与顺序
本规格与 [Spec 015](015-daemon-concurrency.md) 同触 `daemon/`+`core/watcher.*`，须**在其后**开工；
[Spec 017](017-index-memory.md)（内存精简/模式）又依赖本规格的默认值变更（条目数上升），
故整体顺序为 **015 → 016 → 017**。

## Deliverable
`core/watcher.cpp`/`core/watcher.h`、`core/search.*`（`totalBytes`）、`tests/`、`scripts/`（用例）、文档

## Evidence
（待实现后填充）

### 复现证据（立项依据，隔离环境 + `nc -U`/CLI）
```
config index_hidden = 0
初始 count(hidprobe) = 0
新建隐藏文件后 count(hidprobe) = 0        # $HOME 根目录（事件合并 -> CREATE 分支，被过滤）
隐藏目录内 count(inside) = 0
== 子目录内新建隐藏文件 ==  count(hidmv3) = 1   # 错误
== 可见 rename（temp -> vis2.md）==  count(vis2) = 0   # 错误
stats.size = 8388608.0 TB（2^63，溢出）
```

## 依赖
本规格与 [Spec 015](015-daemon-concurrency.md) 同触 `daemon/`+`core/watcher.*`；**须在 015 合入后
再开工**以避免文件冲突（015 已在进行中）。
