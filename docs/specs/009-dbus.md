# Lsearch Spec 009 — D-Bus 集成（桌面门面 + 按需激活）

Status: **Done**（已按 Oracle 设计评审修订）

## Why
麒麟（UKUI）等 Linux 桌面的服务发现、按需启动与集成均以 D-Bus 为标准。Lsearch 目前只提供
私有 Unix socket 明文协议：CLI/TUI/GUI/MCP 可用，但桌面与其它信创应用无法以标准方式发现、
调用或按需拉起 `lsearchd`，也没有标准自启姿势。D-Bus 作为**并存的桌面门面**（不替换 socket），
使 lsearchd 成为桌面一等服务，并为后续「桌面全局搜索提供者 / 文件管理器集成」铺路。

## What Changes
- 新增 `dbus/` 前端：**薄桥接进程 `lsearch-dbus`**（复用 `ipc/client`，与 CLI/TUI/GUI/MCP
  同构；`core/`、`daemon/` 与 socket 协议**零改动**。注意：桥接自身新增构建期依赖
  `libdbus-1-dev` 与运行期 `libdbus-1-3`/`dbus-libs`）
- 在 **session bus** 注册服务 `com.lsearch.Daemon`（对象 `/com/lsearch/Daemon`，接口
  `com.lsearch.Daemon1`）：
  - 方法 `Search(s query, u limit, u offset, s sort, s kind) -> s results_json`
    （JSON 封套与 MCP `search_files` 同构：`results` + `page`）
  - 方法 `Stats() -> a{sv}`（具体类型见 R2）
  - 方法 `Rebuild()`（异步触发、立即返回；桥接侧节流，见 R4）
  - 方法 `Version() -> s`
  - 标准接口：`org.freedesktop.DBus.Introspectable.Introspect`、`org.freedesktop.DBus.Peer.Ping`
- **按需激活**：`com.lsearch.Daemon.service`（`Exec=<CMAKE_INSTALL_FULL_BINDIR>/lsearch-dbus`，
  由 `configure_file` 生成，不硬编码路径）安装至 `/usr/share/dbus-1/services/`
- 打包：deb/rpm 纳入 `lsearch-dbus` 与 `.service`；依赖增加 `libdbus-1-3`（deb）/`dbus-libs`（rpm）
- 文档：README / architecture / 打包说明

## Requirements

#### Requirement 1 — 桥接架构（不侵入 core）
`lsearch-dbus` 仅通过 `ipc/client` 访问 lsearchd；`core/`、`daemon/` 不改动。桥接为单线程事件
循环（`poll` + `dbus_connection_read_write_dispatch`），阻塞上限受 `connectOrSpawn` 的 10s 预算约束。

#### Requirement 2 — 接口语义
- `Search`：参数与结果语义与 MCP `search_files` 一致（`re:` 正则、limit 钳制 [1,200]、over-fetch
  分页、basename 匹配）；返回 JSON 封套同构
- **注入防护（关键）**：`query` 必须**拒绝任何 C0 控制字符（<0x20）**（与 MCP 的
  `mcp_query_control_chars_rejected` 同规则），否则 `\n` 会注入 socket 行协议；`sort`/`kind`
  仅接受白名单值，未知值直接 `InvalidArgs`（不转发）
- `Stats`：`a{sv}`，键与 daemon `stats` 一致，类型固定为 `u`（files/dirs/scan_files/scan_dirs）、
  `t`（size/uptime）、`b`（rebuilding）、`s`（roots）
- 错误：非法参数 / 非法正则 / 控制字符 → `com.lsearch.Error.InvalidArgs`；daemon 不可达（含
  首次全量扫描未完成，socket 尚未绑定）→ `com.lsearch.Error.DaemonUnavailable`，消息含可操作
  重试提示（如 "first full scan in progress; retry in ~10s"）；`Rebuild` 过频 →
  `com.lsearch.Error.Busy`
- `Version`：返回桥接/协议版本字符串

#### Requirement 3 — 激活与生命周期
- `.service` 安装后，无桥接进程时标准调用可触发 **D-Bus 激活**并成功返回
- 桥接以 `DBUS_NAME_FLAG_DO_NOT_QUEUE` 请求名字；`EXISTS` 时安静退出（避免排队僵尸）
- 桥接常驻（v1 不做 idle-exit）；SIGTERM/SIGINT 干净退出（释放名字）；**不触碰 lsearchd**
- **会话总线断开**（`Disconnected` 或连接失效）时必须退出 0，以便下次调用重新激活
- session bus 不可用时：启动失败、stderr 可操作错误、退出码非 0、不留僵尸进程
- 桥接被 kill 后：下一次调用可再次激活；**lsearchd 不得被重启**（复用已运行实例）

#### Requirement 4 — 安全边界
仅本用户可达（session bus 天然隔离）；暴露面 = 只读查询 + `Rebuild` + `Version`；**不暴露**
shutdown/set-paths/set-excludes/set-opts/add-path/remove-path。`Rebuild` 桥接侧做简单节流
（如 5s 内重复调用返回 `Busy`），避免被同用户进程刷成全量重扫。

#### Requirement 5 — 兼容与回归
socket 协议与既有前端行为不变；`lsearch_tests`、`self-test.sh`、`self-test-mcp.sh` 全绿；
新增 `scripts/self-test-dbus.sh`：
- 使用 `dbus-run-session` 隔离总线；**在启动总线之前**写入
  `$XDG_DATA_HOME/dbus-1/services/com.lsearch.Daemon.service`（Exec 指向 build 目录的绝对路径）
- `PATH` 含 build 目录（保证 `connectOrSpawn` 能 `execlp("lsearchd")`）；隔离 HOME/XDG_*
- 断言必须**证明是激活而非预启动**：调用前无 `lsearch-dbus` 进程且名字无 owner；调用成功后
  进程存在、`ppid == dbus-daemon`、daemon 已起
- kill 桥接后再调用：新桥接 PID ≠ 旧 PID，但 daemon PID 不变
- 错误用例：非法 sort / 空 query / `re:[` / 含 `\n` 的 query → `InvalidArgs`

#### Requirement 6 — 打包
deb：`lsearch-dbus` + `/usr/share/dbus-1/services/com.lsearch.Daemon.service`（含目录），
Depends 增加 `libdbus-1-3`；rpm：`%files` 同上，`Requires: dbus-libs`（并确保
`/usr/share/dbus-1/services` 由 `dbus`/`dbus-common` 提供）。**不加** `SystemdService=`
（默认 session bus 为 dbus-daemon 直接 spawn，非 systemd 激活）。

## 非目标（本期）
- **`IndexChanged` 信号与 2s stats 轮询**（无消费者、唤醒 daemon、有漏检窗口）→ 后续规格
- `org.freedesktop.DBus.Properties` 完整支持（`Version` 用方法代替）
- 桌面全局搜索提供者插件（UKUI/GNOME shell search provider）、文件管理器右键集成 → 后续规格
- 桥接 idle-exit（v1 常驻）
- system bus 与跨用户访问；替换 Unix socket

## Scenario
Given 有 session bus（真实桌面或 `dbus-run-session`，且 service 文件已就位）
When `gdbus call --session --dest com.lsearch.Daemon --object-path /com/lsearch/Daemon --method com.lsearch.Daemon1.Search "report" 20 0 name any`
Then 返回 JSON 封套（与 MCP 同构）且命中预期文件；调用前无桥接进程、调用后桥接被 D-Bus 激活
When daemon 未运行（冷启动）
Then 激活桥接 → 桥接自动拉起 lsearchd；若首次全量扫描未完成则返回 `DaemonUnavailable` +
重试提示（可操作）
When `Stats` / `Version` / `Introspect`
Then 分别返回 `a{sv}`、版本字符串、接口 XML
When `Search` 传非法 sort / 空 query / `re:[` / `"x\nshutdown"`
Then `com.lsearch.Error.InvalidArgs`，桥接与 daemon 不受影响
When kill 桥接进程后再调用
Then D-Bus 重新激活成功（新桥接 PID），且 daemon PID 不变
When 会话总线断开
Then 桥接退出 0

## Task
- [x] Oracle 设计评审并据其修订本规格
- [x] 安装 `libdbus-1-dev`；CMake：`find_package(PkgConfig)` + `pkg_check_modules(DBUS dbus-1)`，
      新增 `lsearch_dbus_lib`（纯逻辑：参数映射/校验，可单测）+ `lsearch-dbus` 可执行
- [x] `dbus/`：注册名字（DO_NOT_QUEUE）、方法分发、Introspect/Peer、断线退出、信号处理
- [x] `com.lsearch.Daemon.service.in` + `configure_file` + install
- [x] `scripts/self-test-dbus.sh`（dbus-run-session + gdbus，含"证明激活"断言）
- [x] 打包集成（deb/rpm 依赖与文件）+ 文档（README/architecture）

## Deliverable
`dbus/`、`com.lsearch.Daemon.service`、自测脚本、打包与文档更新

## Evidence
- 构建（C++17，`-Wall -Wextra` 零告警）：
  `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j"$(nproc)"`（`--clean-first` 全量重建无告警）。
  产物 `build/lsearch-dbus`；纯逻辑静态库 `lsearch_dbus_lib`（`dbus/logic.cpp`，不依赖 libdbus，
  可无总线单测）；service 文件经 `configure_file` 生成（`CMAKE_INSTALL_FULL_BINDIR` 展开，不硬编码）。
- 单测 `./build/lsearch_tests` → **334 checks / 0 failures**（Spec 009 新增 67 checks，8 例：
  `dbus_make_search_args_valid`、`dbus_make_search_args_whitelist`、
  `dbus_make_search_args_control_chars`、`dbus_make_search_args_limit_offset_clamp`、
  `dbus_make_search_args_empty_and_regex`、`dbus_map_stats_types`、`dbus_map_stats_missing_keys`、
  `dbus_rebuild_throttle`）。
- 端到端 `./scripts/self-test-dbus.sh -s` → **通过 22 项 / 失败 0 项**（D1–D22，`dbus-run-session`
  + `gdbus call --session` + 隔离 HOME/XDG_*，service 文件在总线启动前写入）：
  - D1–D3 调用前无 `lsearch-dbus` 进程、隔离 socket 不存在、`NameHasOwner=false`（非预启动）；
  - D4 `Search "report" 20 0 name any` 触发按需激活并命中 `AnnualReport.txt`；
  - D5/D6 桥接进程存在，且总线 `GetConnectionUnixProcessID(com.lsearch.Daemon)` 返回的 owner
    PID == 桥接 PID（激活的权威证明）；D7 桥接非测试脚本直接启动；D8 `lsearchd` 由桥接拉起且为
    `build/` 版本（证明激活链）；
  - D9 `Stats` 返回 `a{sv}` 含 `files`/`rebuilding`；D10 `Version` 非空；D11 `Introspect` 返回接口 XML；
  - D12–D15 非法 sort / 空 query / `re:[` / 含 `\n` query → `GDBus.Error:com.lsearch.Error.InvalidArgs`，
    D16 之后 `lsearchd` 仍存活（换行注入防护，MUST）；
  - D17–D20 `kill` 桥接后再次调用成功（重新激活），新桥接 PID ≠ 旧，且 `lsearchd` PID 集合不变；
  - D21/D22 `Rebuild` 首次触发成功、5s 内再次调用 → `com.lsearch.Error.Busy`（桥接侧节流）。
- 回归：`./scripts/self-test.sh -s` → 22/22；`./scripts/self-test-mcp.sh -s` → 29/29。
- 打包实测：deb `dpkg-deb -c` 含 `./usr/bin/lsearch-dbus` 与
  `./usr/share/dbus-1/services/com.lsearch.Daemon.service`，`dpkg-deb -I` Depends 含 `libdbus-1-3`；
  rpm `rpm -qlp` 含 `/usr/bin/lsearch-dbus` + service，`rpm -qpR` 含 `dbus-libs`。

### 与规格字面差异（环境实测，仅此一处）
规格 R3/Task 原要求断言桥接 `ppid == dbus-daemon`。本机 dbus 1.16.2 在服务成功取到 well-known
name 后即回收 activation babysitter（`_dbus_spawn_async_with_babysitter`），服务被 reparent 到
init（/proc 实测 PPid=/init，而非 dbus-daemon；对照组 `/bin/sleep`、`dbus-monitor` 等不取名的
服务 PPID 仍是 dbus-daemon）。故自测以**更强且与 dbus 版本无关**的证据断言激活：调用前无进程/
无 owner，调用后总线 owner PID == 桥接 PID（D3/D5/D6）。激活、kill 后重激活、daemon 不被重启
等其余要求均按规格断言。
