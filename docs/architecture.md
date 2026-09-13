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
| 计数/分页语义 | 协议 v2（Spec 010） | 旧 `search` 早停于请求 limit（TUI/GUI/D-Bus 仍用，快速路径）；`search2`/`count2` 在候选上限（5 万）处停止：命中 ≤cap 时 `total` 精确、页间稳定，>cap 时 `total_capped=1`、`total` 为下界、不承诺 >cap 页稳定；`count2` 免物化免排序 |
| 增量更新 | inotify | 为内存索引中的每个目录加 watch；新目录在 `IN_CREATE` 时递归挂接；`IN_MODIFY/ATTRIB` 触发 re-stat |
| 排除 | 前缀匹配 + 隐藏文件开关 | 默认排除 /proc /sys /dev /run 与 `~/.cache`、回收站 |
| IPC | 明文行协议 | 简单、可用 socat 调试；D-Bus 作为**并存的桌面门面**（Spec 009：`lsearch-dbus` 注册会话总线 `com.lsearch.Daemon`，按需激活，底层仍复用同一 socket 客户端） |
| 并发 | `std::shared_mutex` | 搜索读共享锁、增量写独占锁；SQLite WAL 处理 DB 读写并发 |
| 不变量 | 仅用户家目录 | 默认 `paths = $HOME`，配置极简，避免隐私与首扫过慢 |

## 4. 一次搜索的路径

```
CLI/MCP → Client → socket → lsearchd serveConnection
        → handleRequest("search2 <limit> <dirs> <files> <sort> <under64> <query>")
        → idx.searchEx()（共享锁，并行分块扫描：命中 ≤cap 收集全部 → 精确 total；否则于 cap 停止）
        → "OK <returned> <total> <total_capped>" + 结果行 → socket → 前端渲染/分页
```
TUI/GUI/D-Bus 仍走旧 `search` 命令（`idx.search()` 早停于请求 limit 的快速路径）；
`count2` 走 `idx.countEx()`（免物化）。`capabilities` 供新客户端探测 `search2`，旧 daemon
（无该命令）时客户端自动降级到 `search`。

## 5. 开放问题 / 后续（V2/V3）
- **离线追平**：守护进程停机期间的改动不自动补齐（V1 用 F5 重建兜底）。
- **inotify 上限**：海量目录时需调高 `max_user_watches`；后续可加周期对账。
- **启动恢复**：V1 直接 load SQLite；后续可按目录 mtime 做局部重扫，避免全量重建。
- **Qt GUI**：`core` 与前端已解耦，GUI 仅需新前端，复用 IPC client。
- **D-Bus 后续**（Spec 009 已交付会话总线门面 `lsearch-dbus`）：桌面全局搜索提供者
  （UKUI/GNOME shell）、文件管理器集成、`IndexChanged` 信号与 Properties → 后续规格。
- **多架构打包**：x86_64 + aarch64（飞腾/鲲鹏）交叉编译矩阵。

## 6. 开源参考
- [Fsearch](https://github.com/cboxdoerfer/fsearch)：借鉴索引更新与索引数据结构（C/GTK3）。
- [fd](https://github.com/sharkdp/fd) / [ripgrep](https://github.com/BurntSushi/ripgrep)：并行遍历、忽略规则技巧。
- [fzf](https://github.com/junegunn/fzf)：TUI 交互（模糊匹配、预览）体验参考。
- plocate/mlocate：数据库式检索的更新策略对照。
