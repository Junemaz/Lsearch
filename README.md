# Lsearch — 面向麒麟 V10 桌面的 Everything 风格文件名搜索

Lsearch 是一款参考 Windows 版 **Everything** 打造的文件名即时搜索工具，专为
麒麟（Kylin V10）等信创桌面环境设计，采用 **C++17 + Qt5 + SQLite + inotify**
技术栈，支持 **TUI 与 GUI** 两种前端（V1 交付 TUI，GUI 预留）。

> Everything 之所以快，是因为 NTFS 的 MFT + USN 日志免费提供了文件名索引；
> Linux 的 ext4 没有等价物，因此 Lsearch 必须**自建索引**：首次全量遍历 +
> 内存热索引 + SQLite 持久化 + inotify 增量更新，实现"即输即搜"。

## 特性（V1）
- 常驻守护进程 `lsearchd`：全量建索引 → SQLite 持久化 → inotify 实时增量 → 内存热索引
- Unix domain socket IPC（明文协议，可用 ncurses/GUI/CLI 共享，脚本/socat 可调试）
- CLI：`lsearch <关键词>` 即时输出路径，支持子串/通配符/`re:` 正则、排序、计数、脚本用 `-0`
- TUI：`lsearch-tui` 输入即搜、方向键浏览、Enter 打开（xdg-open）、F5 重建
- 默认仅索引用户家目录，简单纯文本配置（`~/.config/lsearch/lsearch.conf`）
- 自动拉起守护进程：CLI/TUI 连不上 socket 时会自动把 `lsearchd` 拉起来

## 构建
```bash
# 依赖：cmake、g++、libsqlite3-dev、libncurses-dev
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
```
产物：`build/lsearchd`、`build/lsearch`、`build/lsearch-tui`、`build/lsearch-mcp`、`build/lsearch_tests`

## 快速开始
```bash
# 1) 启动守护进程（自动生成配置并全量建索引；也可直接运行 lsearch 自动拉起）
lsearchd --foreground            # 前台调试；默认后台守护化

# 2) 搜索
lsearch report                   # 子串匹配，大小写不敏感
lsearch '*.pdf'                  # 含 * ? 时切换为通配符
lsearch 're:^report-\d{4}\.pdf$' # re: 前缀切换为 ECMAScript 正则（大小写不敏感）
lsearch --count a                # 仅计数
lsearch -d '报告'                # 只显示目录
lsearch -s size -S 'log'         # 按大小排序，带详情列
lsearch --stats                  # 索引统计

# 3) 交互界面
lsearch-tui                      # 输入即搜；↑↓ 选择，Enter 打开，F5 重建，F6 排序，F7 反序，Esc 清空，Ctrl+Q 退出
```

> TUI 的完整操作与排障见 [docs/tui-manual.md](docs/tui-manual.md)。

## 自测
一键自测核心流程（含建索引 / 搜索 / inotify 增量 / 停机补齐 / 关闭）：
```bash
./scripts/self-test.sh          # 自动构建后跑全部 22 项
./scripts/self-test.sh -s       # 跳过构建，直接用现有 build/
```
MCP 前端自测（stdio JSON-RPC，S1–S11；需 python3）：
```bash
./scripts/self-test-mcp.sh      # 自动构建后跑全部 29 项
./scripts/self-test-mcp.sh -s   # 跳过构建
```
TUI 自动化自测（tmux 伪终端注入按键并断言画面；需装 tmux）：
```bash
./scripts/self-test-tui.sh      # 输入即搜 / 通配符 / F5 重建 / Esc 清空 / Ctrl+Q 退出
```

## 配置（简单纯文本，首次运行自动生成）
```ini
# ~/.config/lsearch/lsearch.conf
paths = /home/user, /data        # 索引根路径，逗号分隔；默认仅用户家目录
excludes = /proc,/sys,/dev,/run  # 排除前缀
index_hidden = 0                 # 是否索引隐藏文件
follow_symlinks = 0              # 是否跟随符号链接
```
管理命令：`lsearchd --add-path /data`、`lsearchd --remove-path /data`、
`lsearchd --rebuild`、`lsearchd --shutdown`（已运行时自动转发给现有进程）。

## 架构
```
lsearchd 守护进程 ── Unix socket IPC ──┬─ GUI（Qt5，已完成，直接复用 core）
  ├ core/indexer 并行全量遍历           ├─ TUI（ncurses，V1 已交付）
  ├ core/watcher inotify 增量           └─ CLI（lsearch）
  ├ core/db      SQLite 持久化
  └ core/search  内存热索引
```
详见 [`docs/architecture.md`](docs/architecture.md)。

## 已知限制（V1）
- 守护进程离线期间的改动不自动追平（默认幂等：重启加载 SQLite，可 F5 重建）
- inotify 有 watch 上限：目录极多时需调高
  `/proc/sys/fs/inotify/max_user_watches`，否则仅告警不阻塞
- 匹配仅针对**最终文件/文件夹名（basename）**做子串/通配符/`re:` 正则（完整路径不参与命中）；全文内容搜索不在范围
- `re:` 支持 ECMAScript 正则；病态模式可能长时间占用搜索线程（本地 DoS），仅影响本机查询（详见 [Spec 008](docs/specs/008-regex.md#已知限制)）
- TUI 需在真实终端（SSH/本地控制台）运行

## 路线图
- [x] V1：lsearchd + CLI + TUI + inotify + SQLite + 单元测试 + 打包脚本
- [x] V2：Qt5 GUI —— 实时搜索框、结果表格、双击打开、系统托盘常驻、深色现代主题
- [x] V2：索引管理页 —— GUI 内增删索引根路径/排除前缀/选项（隐藏文件、符号链接）、一键重建
- [x] V3：正则（`re:` 前缀，ECMAScript，大小写不敏感）
- [ ] V3：多架构 CI、D-Bus 集成

## 开源参考
[Fsearch](https://github.com/cboxdoerfer/fsearch)、[fd](https://github.com/sharkdp/fd)、[fzf](https://github.com/junegunn/fzf)、plocate 等（见 architecture 文档）。

## License
MIT
