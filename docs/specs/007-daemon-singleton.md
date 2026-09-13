# Lsearch Spec 007 — 守护进程单例锁（并发冷启动竞态修复）

Status: **Done**

## Why
冷启动存在竞态：两个客户端同时拉起 `lsearchd` 时，双方都可能通过「已运行探测」
（`daemon/main.cpp` 的 probe），随后各自进入 `Daemon::run()`；而 `run()` 在 `bind` 前执行
`::unlink(sock)` —— 后启动者会**偷走先启动者的 socket**，两个 daemon 同时存活并写同一个
SQLite（WAL 虽能承受写冲突，但语义重复、管理命令可能丢失）。MCP 前端（Spec 006）会放大
该风险，因为代理会并发拉起多个 stdio server。

## What Changes
- `daemon/main.cpp`：在「已运行探测」之后、daemonize（fork）之前获取**实例锁**：
  `flock(LOCK_EX | LOCK_NB)` 锁定 `<sock_path>.lock`（同目录、0700、0600、O_CREAT）。
  持锁 fd 由 fork 后的子进程继承（父进程退出不释放），daemon 退出时随 fd 关闭自动释放，
  无陈旧锁问题。
- 锁被占用 = 另一实例正在启动/运行：打印提示并以**退出码 0** 退出（非错误）。
- 锁文件无法创建/打开：明确报错并退出 1（不静默降级为无锁运行）。

## Requirements

#### Requirement 1 — 单例互斥
任一时刻至多一个 `lsearchd` 实例进入 `init`/`run`；竞争失败者在触碰数据库之前退出。

#### Requirement 2 — 锁的落点与权限
锁文件与 socket 同目录（缺失时以 0700 创建），命名为 `<sock_path>.lock`，
`open(O_RDWR|O_CREAT|O_CLOEXEC, 0600)` + `flock(LOCK_EX|LOCK_NB)`。

#### Requirement 3 — 生命周期与继承
持锁 fd 在守护化 fork 后由子进程持有；进程退出（含异常终止）即释放，无陈旧锁。

#### Requirement 4 — 失败语义
`busy` → stderr 提示「单例锁被占用」，退出 0；其他打开失败 → stderr 错误，退出 1。

#### Requirement 5 — 无回归
既有单元测试与 `scripts/self-test.sh` 全部通过；正常单实例启动/搜索/增量/关闭行为不变。

## Scenario: 并发冷启动只有一个实例
Given 无 daemon 运行
When 用 `flock` 占住 `<sock>.lock` 后执行 `lsearchd --foreground`
Then 进程在 5s 内退出（非 timeout 杀死），stderr 含「单例」提示，未创建/未偷 socket
When 同时启动两个 `lsearchd --foreground`
Then 恰好一个存活，另一个退出；socket 正常服务（`lsearch --stats` 可查）
When `lsearchd --shutdown`
Then socket 清理干净、无残留进程

## Task
- [x] `daemon/main.cpp`：单例锁 `acquireInstanceLock` + 启动路径接入
- [x] `scripts/self-test.sh`：新增「单例锁（并发冷启动）」断言
- [x] `docs/specs/README.md`、`README.md` 更新

## Deliverable
`daemon/main.cpp`、`scripts/self-test.sh`、本规格

## Evidence
- 构建：`cmake --build build -j"$(nproc)"` 通过（`lsearchd` 重新链接）
- 单测：`./build/lsearch_tests` → 55 checks / 0 failures
- 端到端：`./scripts/self-test.sh -s` → **22/22 通过**，其中新增 4 项：
  - 锁被占用时新实例快速退出（未偷 socket）
  - 并发启动恰好一个实例存活
  - 存活实例正常服务（stats）
  - 单例场景 shutdown 干净
- 过程记录：首轮自测暴露的是测试脚本缺陷（`flock -c` 的子进程在 `kill` 后仍持锁 ~2.7s），
  已将持锁方式改为脚本自身 fd（`exec 9>` + `flock -n 9`），测试变为确定性；实现本身行为正确。
