# Lsearch Spec 014 — 最小 CI（GitHub Actions）

Status: **In Progress**

## Why
Spec 011/012/013 都把"不引入托管 CI"列为非目标；如今自测已达 12 套（单测 + 自测 +
外部验证），仓库也已公开——回归不该再依赖人工记忆。现有脚本天然支持 **SKIP 语义**
（缺 npx / 网络 / 第三方工具时 `exit 0`），正好适配 CI，不需要为 CI 另造测试。

## What Changes
新增 `.github/workflows/ci.yml`，在 **push 到 main** 与 **所有 PR** 上运行，两个 job：

- **`core`（必需）**：装依赖 → 配置 + 构建（含 GUI / TUI）→ 跑
  `lsearch_tests`、`self-test.sh`、`self-test-ipc.sh`、`self-test-mcp.sh`、
  `self-test-dbus.sh`、`self-test-ipc-external.sh`、`self-test-ipc-diff.sh`、
  `self-test-ipc-matrix.sh`、`self-test-tui.sh`
- **`mcp-external`（必需 vs 允许失败，见"待定"）**：跑
  `self-test-mcp-conformance.sh`、`self-test-mcp-inspector.sh`、`self-test-mcp-matrix.sh`、
  `self-test-mcp-replay.sh`（合成 + 真实 fixture）；依赖 npx / 网络，脚本在缺失时 **SKIP**

并发取消旧 run（`concurrency`），避免堆积。

## Requirements

#### Requirement 1 — 触发与失败语义
workflow 在 push 到 `main` 与 PR 上触发；任一**必需** job 失败即整体失败；SKIP 不算失败。

#### Requirement 2 — `core` job 依赖齐全可构建
Ubuntu 环境安装：`build-essential cmake libsqlite3-dev libdbus-1-dev libncurses-dev
qtbase5-dev python3 tmux netcat-openbsd dbus libglib2.0-bin`。
（`qtbase5-dev` 是**必需**的：`CMakeLists.txt` 的 `framelesswindow` 目标在
`if(BUILD_GUI)` 之外无条件链接 `Qt5::Widgets`，故缺少 Qt5 时 configure 直接失败。）

#### Requirement 3 — SKIP 语义在 CI 中不误报
缺少 npx / 网络 / 第三方工具（`nc`、`gdbus`、`tmux`）时，对应套件按既有 SKIP 语义
`exit 0`；CI 日志须能看出跳过了什么。

#### Requirement 4 — 不回归且与本地一致
CI 覆盖的套件集合与本地一致（若有意差异，须在 README 与规格中列明理由）。
首轮 CI 必须能复现本地结果（单测 552/0；各套通过数一致）。

#### Requirement 5 — 文档同步
README 增 CI 徽章 + "CI 覆盖哪些套件 / 如何在本地复现"；`docs/architecture.md` 的
协议面验证章节与 `docs/specs/README.md` 提及 CI（遵循文档同步全局规则）。

#### Requirement 6 — 不改产品行为
仅新增 CI 与（可选的）本地预演脚本；不改任何运行时行为与协议。

## 待定（实现前定夺）
- **`mcp-external` job 的失败语义**：必需（红即红，网络抖动会误伤）vs
  `continue-on-error`（只作信号）。(a)/(b) 影响仓库"绿"的含义。
- **`self-test-tui.sh`（tmux 伪终端）是否入 `core`**：能覆盖 TUI 但存在偶发抖动风险。

## 非目标
- 多架构 / 交叉编译（aarch64 等，见 Spec 004 的后续项）
- deb/rpm 打包与发布（后续规格）
- 仓库设置类（分支保护、required checks、review 规则）——需管理员权限，另议

## 已知问题（本规格记录）
- `CMakeLists.txt:104-110` 的 `framelesswindow` 在 `BUILD_GUI` 之外无条件链接
  `Qt5::Widgets` → `-DBUILD_GUI=OFF` 仍会因找不到 Qt5 而 configure 失败。
  CI 通过安装 Qt5 规避；是否顺带修 CMake（把该目标纳入 Qt5 检测）见 Task。

## Scenario
Given 一个会破坏单测的提交
When push / 开 PR
Then `core` job 失败，PR 变红
Given 一个正常提交
When CI 运行
Then `core` 全绿；`mcp-external` 按既定失败语义给出结果（网络/npx 缺失时 SKIP 而非 FAIL）
Given 本地开发者
When 按 README 的"CI 本地复现"步骤执行
Then 得到与 CI 相同的套件与通过数

## Task
- [ ] 定夺两个待选项（mcp-external 失败语义、TUI 是否入 core）
- [ ] `.github/workflows/ci.yml`（两 job + concurrency + 依赖安装）
- [ ] （可选）`scripts/ci-local.sh`：本地一键复现 CI 步骤
- [ ] （可选）修 `CMakeLists.txt` 的 Qt5 无条件链接，使 `-DBUILD_GUI=OFF` 可用
- [ ] README 徽章 + 覆盖说明；架构/清单同步
- [ ] Evidence：首轮 CI run 链接 + 本地预演结果；转 Done

## Deliverable
`.github/workflows/ci.yml`、README/文档更新、（可选）`scripts/ci-local.sh`

## Evidence
（待实现后填充）
