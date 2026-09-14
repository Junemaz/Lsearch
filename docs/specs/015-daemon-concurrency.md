# Lsearch Spec 015 — 守护进程并发安全（重建 × 管理命令竞态）

Status: **Done**

## Why
CI（[Spec 014](014-minimal-ci.md)）上线后**首轮就抓到真实崩溃级竞态**（非测试问题）：

- 证据（CI run `34853357610` 的 `core` 日志）：
  ```
  PASS  v13 add-path 合法目录 -> OK
  PASS  v13 合法路径写入 lsearch.conf 恰好一次
  FAIL  v13 remove-path -> OK b''                     ← remove-path 无响应
  ConnectionRefusedError: [Errno 111] Connection refused
  IPC 负形状/边界矩阵：通过 55 项，失败 1 项
  ```
  矩阵用例先 `add-path <dir>`（触发**异步全量重建**）紧接 `remove-path <dir>`；CI 慢机上重建仍在
  进行，`remove-path` 处理期间 **daemon 直接死亡**（客户端连接被拒）。
- 代码层（`daemon/daemon.cpp`）：`rebuildAsync()`（`startRebuild()` 拉起的 detached 线程）与
  `remove-path` 处理都会调用 `restartWatcher()`（`watcher_.stop()/start()`）并写索引/DB；
  `startRebuild()` 的 `rebuildM_` **只保护 `rebuilding_` 标志**，watcher 生命周期与索引/DB 变更
  **没有互斥**。本地因重建瞬间完成而不复现，CI 慢机把它稳定暴露。
- 影响面：`add-path`/`remove-path`/`set-paths`/`set-excludes`/`set-opts`/`rebuild` 都会触发重建，
  任一与重建生命周期重叠即可致**守护进程崩溃**（可用性缺陷；非安全边界问题）。

## What Changes
- 串行化"重建生命周期"与"watcher 生命周期 + 索引/DB 变更"（统一互斥或以取消/等待握手收敛），
  消除对 `Watcher` 与索引/DB 的并发可变访问。
- 明确管理命令与重建的交互契约：并发调用**幂等、可响应、不崩溃**；重建可被管理命令安全打断/重启。
- 以**已存在的矩阵用例**（`add-path` → `remove-path`）作为回归；必要时增加延迟注入以在快机上也能覆盖。

## Requirements

#### Requirement 1 — 不崩溃、不丢连接
在重建进行中执行 `remove-path` / `add-path` / 其他管理命令时，daemon 必须存活：后续
`ping`/`stats` 正常响应，命令返回 `OK`/`ERR`，不得出现连接被拒或空响应。

#### Requirement 2 — 不死锁、不长时间阻塞
不得引入死锁；管理命令在重建期间仍须在**合理时间内**返回（不得等整个全量扫描结束）；
搜索读路径不得被重建长时间独占阻塞。

#### Requirement 3 — 回归与不变量
- 现有 12 套自测 + 单测 + conformance 全绿；IPC 负形状矩阵 61/61 稳定（含慢机）。
- 新增针对本竞态的回归（矩阵用例或单测），在**注入延迟/大目录**下也能稳定复现与通过。
- 既有不变量保持：shutdown 回收全部连接线程；单例锁语义不变。

#### Requirement 4 — 文档同步
`docs/architecture.md` 记录重建与 watcher 的并发契约；014/013 交叉引用；README/规格清单同步。

## 非目标
- 不改协议语义与命令行为；不做性能重写；不引入线程池/事件循环级重构（除非 R1/R2 必需）。

## Scenario
Given 一个正在全量重建的隔离 daemon
When 立即发送 `remove-path <正在重建的根路径>`
Then daemon 存活，`remove-path` 返回 `OK`，随后的 `ping`/`stats` 正常
When 循环 `add-path` / `remove-path` 同一路径 N 次（重建并发）
Then 全部 `OK`，无崩溃、无残留进程、配置落盘正确
When 在重建期间执行搜索
Then 搜索仍能返回（不被重建长时间阻塞）

## Task
- [x] 本地可复现：注入慢重建（大目录或人为延迟）后触发 `add-path`→`remove-path`
- [x] 串行化修复（锁序清晰、无死锁；管理命令不等待整个扫描）
- [x] 回归测试（复用/强化矩阵用例，或补单测）
- [x] 全量复跑 + 文档同步 + Evidence → Done
- [ ] CI 复核：工作流目前只在 PR #9 中；**先合本修复、再合 #9**，届时 #9 的 checks 基于
      「分支 + 已含修复的 main」重跑即为绿（见 Evidence 末尾说明）

## Deliverable
`daemon/`（及必要的 `core/watcher.*`）的并发修复、回归测试、文档更新

## Evidence

### 根因（5 条，全部来自代码审计）
`rebuildAsync()` 运行在 **detached** 线程，与管理命令并发改动共享状态：
1. **watcher 生命周期**：两者都调 `restartWatcher()` → `watcher_.stop()/start()` 无互斥；
   同一 inotify 线程并发 `join()` + 双 `close(fd_)` ⇒ **segfault**（CI 崩溃点）。
2. **SQLite**：唯一 `sqlite3*` 被 `doFullScan()`（`clearFiles`/`upsert`）与
   `remove-path`/`applyWatch`（`removeSubtree`）跨线程写（sqlite3 单连接非线程安全）⇒ UB。
3. **`cfg_`**：扫描遍历 `cfg_.paths` 时管理命令改写 `cfg_.paths/excludes/flags` ⇒ 数据竞争/迭代器失效。
4. 重建期间的第二次 `startRebuild()` 静默 no-op ⇒ 配置变更被丢弃。
5. detached 线程可越过 `Daemon` 生命周期（违反仓库不变量）。

### 修复
- **锁序固化**（`daemon/daemon.h` 注释）：`rebuildM_ → maintM_ → dbM_ → idxLock_`；
  `applyWatch`（事件线程）只取 `dbM_ → idxLock_`，绝不取 `maintM_`——否则
  `restartWatcherLocked()` 持 `maintM_` 并 join 事件线程时自锁。
- **可 join 线程 + 协作取消**：`rebuildThread_` + `rebuildCancel_` + `rebuildCv_` + 原子
  `rebuilding_`；每个管理命令在改动状态前 `cancelAndWaitRebuild()`。取消在 `fullScan`
  的 readdir 循环、`popClaim`（`wait_for` 50 ms 轮询）与 `commitScan`（每 256 条 upsert）处
  被观察 ⇒ 等待有界（毫秒级，不等待完整扫描）。
- **发布与恢复**：仅未被取消才发布（`dbM_`/`idxLock_`，**不持** `maintM_`，只读命令仍可响应）；
  `commitScan` 先 `BEGIN` 再 `clearFiles()`，取消即整体回滚；随后仅在 `maintM_` 下重启 watcher。
- **生命周期**：`run()` / `~Daemon` 均 `cancelAndWaitRebuild()` + `watcher_.stop()`；
  daemon 中已无 `.detach()`。
- **测试钩子**：`LSEARCH_SCAN_DELAY_US`（`core/indexer.{h,cpp}` 内声明）——默认未设置 = 0，
  仅增加每目录扫描耗时、不影响任何结果；用于让竞态在快机上稳定复现。

### 复现与验证
- **基线二进制复刻 CI 签名**：以 `git show HEAD:daemon/daemon.cpp` + 新 core 构建基线
  `build/lsearchd`，跑强化后的矩阵 → 与 CI run `34853357610` **完全同形**：
  `FAIL v13 remove-path -> OK b''` + `ConnectionRefusedError: [Errno 111]` + 55/61。
- **修复后我方独立复现**（隔离 `HOME/XDG_*` + `LSEARCH_SCAN_DELAY_US=4000` + 80×25 `slowroot`，
  `add-path` → 立即 `remove-path` 循环 20 轮）：`RESULT: iters=20 crashes=0`，daemon 存活。
- 代理另跑：`repro.sh 20`（带延迟）与 `repro.sh 20`（不带延迟、大目录）均 `crashes=0`。

### 回归（修复后，本机）
```
cmake --build build            clean（无告警）
lsearch_tests                  552 checks, 0 failures
self-test / ipc / mcp / dbus   25 / 18 / 37 / 22
ipc-external / ipc-diff        7 / 13
ipc-matrix                     61/61 ×3 连续（含本竞态并发回归用例）
运行前后 pgrep -x lsearchd     仅真实守护进程 455627
```

### 不变量
- shutdown 回收全部连接线程与新增的重建线程；daemon 中无 `.detach()`
- 单例锁语义未变（`daemon/main.cpp` 未改，`self-test` 25/25 含并发冷启动用例）
- 无全局锁跨整段扫描；搜索只取 `idxLock_`

### CI 复核说明
`.github/workflows/ci.yml` 目前只存在于 **未合并** 的 [Spec 014](014-minimal-ci.md) PR 中，
故本修复所在分支不会触发 CI。合并顺序建议：**先合并本修复，再合并 SPEC 014 的 PR**——
后者届时基于「其分支 + 已含本修复的 main」重跑 checks，`core` 即为绿（这正是 CI 抓到本缺陷
→ 修复 → CI 转绿的闭环）。
