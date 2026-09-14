# Lsearch Spec 015 — 守护进程并发安全（重建 × 管理命令竞态）

Status: **In Progress**

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
- [ ] 本地可复现：注入慢重建（大目录或人为延迟）后触发 `add-path`→`remove-path`
- [ ] 串行化修复（锁序清晰、无死锁；管理命令不等待整个扫描）
- [ ] 回归测试（复用/强化矩阵用例，或补单测）
- [ ] 全量复跑 + CI 转绿
- [ ] 文档同步 + Evidence → Done

## Deliverable
`daemon/`（及必要的 `core/watcher.*`）的并发修复、回归测试、文档更新

## Evidence
（待实现后填充）

### CI 证据（立项依据，run 34853357610 / core）
```
FAIL  v13 remove-path -> OK b''
Traceback (most recent call last):
  File "<stdin>", line 350, in <module>
  File "<stdin>", line 286, in cfg_get
  File "<stdin>", line 22, in talk
ConnectionRefusedError: [Errno 111] Connection refused
IPC 负形状/边界矩阵：通过 55 项，失败 1 项，SKIP 0 项
```
（同一 run 的 `mcp-external` 亦挂起，已由 Spec 014 的 `timeout` 护栏覆盖。）
