# Lsearch 架构设计

## 1. 目标与核心认知

对标 Windows Everything：**即时**（即输即搜）的文件名搜索。Everything 之快源自
NTFS 的 MFT + USN 日志——文件名索引由文件系统免费维护。Linux/ext4 没有等价物，
因此 Lsearch 必须**自建索引**。由此得出两条设计原则：

1. 搜索本身不是难点；**索引的构建速度、增量更新、内存占用**才是核心工程点。
2. "适配麒麟"的难点不在 UI，而在**文件系统监控与索引策略**的稳定性。

## 2. 进程与模块划分

```
                ┌───────────────────────────────┐
                │  lsearchd 常驻守护进程 (用户级)  │
                │  ├ core/indexer  并行全量遍历    │
                │  ├ core/watcher  inotify 增量    │
                │  ├ core/db       SQLite 持久化   │
                │  └ core/search   内存热索引       │
                └──────────────┬────────────────┘
                               │ Unix domain socket（明文行协议）
        ┌──────────────┬───────┴───────┬──────────────┐
        ▼              ▼               ▼              ▼
   CLI (lsearch)  TUI (lsearch-tui)  GUI (Qt5)   MCP (lsearch-mcp)
                                                       │
                              D-Bus 门面 lsearch-dbus ──┘  （会话总线 com.lsearch.Daemon，
                              按需激活；桥接复用 ipc/client，不触碰 core）
```

**守护进程 + 多前端**的原因：索引只建一次、实时监控只跑一份，多端共享同一索引；
TUI 只实现核心功能、GUI 后续接入，均不改动 `core`。

## 3. 关键技术决策

| 部分 | 决策 | 说明 |
|---|---|---|
| 索引存储 | SQLite（WAL） | `files(path PK,name,size,mtime,is_dir,inode)` + `meta` 表；持久化用于守护进程重启后快速恢复，无需重扫 |
| 内存热索引 | 排序数组 + 路径哈希 | 守护进程常驻，查询在主进程内零 IPC；预计算小写 name/path 加速匹配 |
| 检索语义 | 大小写不敏感子串；含 `* ?` 转通配符；`re:` 前缀转 ECMAScript 正则；可选 `under` 子树过滤 | 仅匹配 basename（不匹配完整路径）；支持按 name/path/size/mtime 排序；`under` 为绝对路径前缀（原始字节、大小写敏感，`path==under || startsWith(path, under+"/")`）；正则为回溯引擎，病态模式可能长时间占用搜索（见 Spec 008「已知限制」） |
| 计数/分页语义 | 协议 v2/v3（Spec 010/012） | `search`/`search2`/`count2` 在候选上限（5 万）处停止：命中 ≤cap 时 `total` 精确、页间稳定，>cap 时 `total_capped=1`、`total` 为下界、不承诺 >cap 页稳定；`count2` 免物化免排序。`search3`（Spec 012）与 `search2` 同语义，仅把结果行 path 转义以保帧完整；客户端探测到 `search3` 后默认路径也走它，否则回退 `search2`/`search`（旧 daemon 字节不变） |
| 增量更新 | inotify | 为内存索引中的每个目录加 watch；新目录在 `IN_CREATE` 时递归挂接；`IN_MODIFY/ATTRIB` 触发 re-stat |
| 排除 | 前缀匹配 + 隐藏文件开关 | 默认排除 /proc /sys /dev /run 与 `~/.cache`、回收站 |
| IPC | 明文行协议 | 简单、可用 socat 调试；D-Bus 作为**并存的桌面门面**（Spec 009：`lsearch-dbus` 注册会话总线 `com.lsearch.Daemon`，按需激活，底层仍复用同一 socket 客户端） |
| 并发 | `std::shared_mutex` | 搜索读共享锁、增量写独占锁；SQLite WAL 处理 DB 读写并发 |
| 不变量 | 仅用户家目录 | 默认 `paths = $HOME`，配置极简，避免隐私与首扫过慢 |

## 4. 一次搜索的路径

```
CLI/MCP → Client → socket → lsearchd serveConnection
        → handleRequest("search3 <limit> <dirs> <files> <sort> <under64> <query>")
        → idx.searchEx()（共享锁，并行分块扫描：命中 ≤cap 收集全部 → 精确 total；否则于 cap 停止）
        → "OK <returned> <total> <total_capped> esc=1" + 转义结果行 → socket → 前端解码渲染/分页
```
`search3`（Spec 012）结果行 path 字段转义（`\\ \t \n \r`）以保帧完整（Linux 文件名可含这些字节）；
`capabilities` 供客户端探测 `search3`/`search2`，无 `search3` 时降级 `search2`，无 `search2` 时回退
旧 `search`（`idx.search()` 早停于请求 limit 的快速路径，仅旧 daemon 场景）。`count2` 走
`idx.countEx()`（免物化）。单行请求上限 1 MiB，超限 `ERR line too long` 并断开该连接。

## 5. 开放问题 / 后续（V2/V3）
- **离线追平**：守护进程停机期间的改动不自动补齐（V1 用 F5 重建兜底）。
- **inotify 上限**：海量目录时需调高 `max_user_watches`；后续可加周期对账。
- **启动恢复**：V1 直接 load SQLite；后续可按目录 mtime 做局部重扫，避免全量重建。
- **Qt GUI**：`core` 与前端已解耦，GUI 仅需新前端，复用 IPC client。
- **D-Bus 后续**（Spec 009 已交付会话总线门面 `lsearch-dbus`）：桌面全局搜索提供者
  （UKUI/GNOME shell）、文件管理器集成、`IndexChanged` 信号与 Properties → 后续规格。
- **多架构打包**：x86_64 + aarch64（飞腾/鲲鹏）交叉编译矩阵。

## 6. 协议面测试与外部验证（Spec 011）

协议表面（MCP / IPC / D-Bus）的“完成”必须包含**至少一个外部验证器**（AGENTS.md 全局规则），
自写 driver 只能证明“在我们假设的客户端下正确”，不能替代规范/真实客户端行为：

- **MCP**：官方 `@modelcontextprotocol/conformance`（`server` 模式，经 supergateway
  stdio→HTTP 桥）、第三方 Inspector CLI、真实客户端（opencode）转录回放；本服务刻意不实现的
  能力（resources/sampling/elicitation/logging/completion/prompts 等，见 Spec 006 非目标）
  以 expected-failures 基线记录理由（`scripts/mcp-conformance-baseline.yml`）。
- **IPC**（Spec 012）：自写 driver `scripts/self-test-ipc.sh`（P1–P18，基线）之外，新增**外部验证**——
  第三方 `nc -U` 直连原始协议（`self-test-ipc-external.sh`）、仅据 `ipc/proto.h` 实现的独立客户端与
  CLI **逐字节差分**（`self-test-ipc-diff.sh`）、负形状/边界矩阵（`self-test-ipc-matrix.sh`）。
- **D-Bus**：`scripts/self-test-dbus.sh`（`dbus-run-session` + `gdbus` 第三方工具 D1–D22）。

详见 [Spec 011](specs/011-test-hardening.md)（MCP：负形状矩阵 / 转录回放 / 外部验证脚本）与
[Spec 012](specs/012-ipc-hardening.md)（IPC：`nc -U` / 独立客户端差分 / 帧完整性 `search3`）。

## 7. 开源参考
- [Fsearch](https://github.com/cboxdoerfer/fsearch)：借鉴索引更新与索引数据结构（C/GTK3）。
- [fd](https://github.com/sharkdp/fd) / [ripgrep](https://github.com/BurntSushi/ripgrep)：并行遍历、忽略规则技巧。
- [fzf](https://github.com/junegunn/fzf)：TUI 交互（模糊匹配、预览）体验参考。
- plocate/mlocate：数据库式检索的更新策略对照。
