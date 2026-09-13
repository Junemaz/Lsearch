# Lsearch Spec 009 — D-Bus 集成（桌面门面 + 按需激活）

Status: **Proposed**

## Why
麒麟（UKUI）等 Linux 桌面的服务发现、按需启动与集成均以 D-Bus 为标准。Lsearch 目前只提供
私有 Unix socket 明文协议：CLI/TUI/GUI/MCP 可用，但桌面与其它信创应用无法以标准方式发现、
调用或按需拉起 `lsearchd`，也没有标准自启姿势。D-Bus 作为**并存的桌面门面**（不替换 socket），
使 lsearchd 成为桌面一等服务，并为后续「桌面全局搜索提供者 / 文件管理器集成」铺路。

## What Changes
- 新增 `dbus/` 前端：**薄桥接进程 `lsearch-dbus`**（复用 `ipc/client`，与 CLI/TUI/GUI/MCP
  同构；`core`/`lsearchd` 零改动、零新依赖）
- 在 **session bus** 注册服务 `com.lsearch.Daemon`（对象 `/com/lsearch/Daemon`，接口
  `com.lsearch.Daemon1`）：
  - 方法 `Search(s query, u limit, u offset, s sort, s kind) -> s results_json`（JSON 封套与
    MCP `search_files` 同构：`results` + `page`）
  - 方法 `Stats() -> a{sv}`（files/dirs/size/uptime/roots/rebuilding/scan_files/scan_dirs）
  - 方法 `Rebuild()`（异步触发，立即返回；重复调用幂等）
  - 信号 `IndexChanged(s reason)`（桥接观察到重建完成时发出）
  - 属性 `Version`（只读；经 `org.freedesktop.DBus.Properties.Get/GetAll`）
- **按需激活**：安装 `com.lsearch.Daemon.service`（`Exec=/usr/bin/lsearch-dbus`）——桌面首次
  调用即由总线拉起桥接；桥接再确保 `lsearchd` 运行（复用 `Client::connectOrSpawn`）
- 打包：deb/rpm 纳入 `lsearch-dbus` 与 `.service`；依赖增加 `libdbus-1-3`（deb）/
  `dbus-libs`（rpm）
- 文档：README / architecture / 打包说明

## Requirements

#### Requirement 1 — 桥接架构（不侵入 core）
`lsearch-dbus` 仅通过 `ipc/client` 访问 lsearchd；`core/`、`daemon/` 与 socket 协议不改动。

#### Requirement 2 — 接口语义
- `Search`：参数与结果语义与 MCP `search_files` 完全一致（`re:` 正则、limit 钳制 [1,200]、
  空查询拒绝、over-fetch 分页、basename 匹配）；返回 JSON 封套同构
- `Stats`：与 daemon `stats` 键一致
- 错误：非法参数 / 非法正则 → `com.lsearch.Error.InvalidArgs`；daemon 不可达（含首次建索引
  超时）→ `com.lsearch.Error.DaemonUnavailable`，消息含可操作重试建议
- `IndexChanged`：桥接在观察到 `rebuilding 1→0` 转换时发出（轻量轮询 stats，周期 ≤2s）

#### Requirement 3 — 激活与生命周期
- 安装 `.service` 后，无桥接进程时标准调用可触发 **D-Bus 激活**并成功返回
- 桥接常驻（持有 bus name）；SIGTERM/SIGINT 干净退出（释放名字），**不触碰 lsearchd**
- session bus 不可用时：启动失败并输出可操作错误（退出码非 0），不产生僵尸进程
- 桥接被 kill 后：下一次调用可再次激活（幂等）

#### Requirement 4 — 安全边界
仅本用户可达（session bus 天然隔离）；暴露面 = 只读查询 + `Rebuild`；**不暴露**
shutdown/set-paths/set-excludes/set-opts/add-path/remove-path（与 Spec 006 只读原则一致）

#### Requirement 5 — 兼容与回归
socket 协议与既有前端行为不变；`lsearch_tests`、`self-test.sh`、`self-test-mcp.sh` 全绿；
新增 `scripts/self-test-dbus.sh`（`dbus-run-session` 隔离总线 + 隔离 HOME/XDG，gdbus 驱动）

#### Requirement 6 — 打包
deb/rpm 含 `lsearch-dbus` 与 `/usr/share/dbus-1/services/com.lsearch.Daemon.service`；依赖声明
正确；不影响既有包内容

## 非目标（本期）
- 桌面全局搜索提供者插件（UKUI/GNOME shell search provider）、文件管理器右键集成 → 后续规格
- system bus 与跨用户访问
- 替换 Unix socket（两者长期并存）
- 除 `Version` 外的完整 Properties 生态

## Scenario
Given 有 session bus（真实桌面或 `dbus-run-session`）
When `gdbus call --session --dest com.lsearch.Daemon --object-path /com/lsearch/Daemon --method com.lsearch.Daemon1.Search "report" 20 0 name any`
Then 返回 JSON 封套（与 MCP 同构）且命中预期文件
When daemon 未运行（冷启动）
Then 激活桥接 → 桥接自动拉起 lsearchd → 调用成功（首次建索引期的错误消息可操作）
When `gdbus call ... Stats`
Then 返回 a{sv} 统计
When `Search` 传非法 sort / 空 query / `re:[`
Then `com.lsearch.Error.InvalidArgs`，桥接与 daemon 不受影响
When kill 桥接进程后再调用
Then D-Bus 重新激活成功

## Task
- [ ] Oracle 设计评审（桥接 vs 内嵌、libdbus vs sd-bus/QtDBus、激活与打包路径）
- [ ] `dbus/`：libdbus-1 桥接（注册名字、方法分发、Properties、信号轮询）
- [ ] CMake target（pkg-config dbus-1）+ install
- [ ] `.service` 激活文件与安装路径
- [ ] `scripts/self-test-dbus.sh`（dbus-run-session + gdbus，隔离环境）
- [ ] 打包集成（deb/rpm 依赖与文件）+ 文档（README/architecture）

## Deliverable
`dbus/`、`com.lsearch.Daemon.service`、自测脚本、打包与文档更新

## Evidence
（未实现，无）
