# Lsearch Spec 012 — IPC 协议加固（外部验证 + 帧完整性修复）

Status: **Done**

## Why
- Spec 011 为 MCP 建立了"外部验证器"制度，但 **IPC 仍只有自写 driver**
  （`scripts/self-test-ipc.sh` P1–P18：raw socket + 自写 Python 客户端），与实现同源盲区。
- 独立探针（本规格立项前，见文末实录）已实证 IPC 在**不受信任文件名**下**帧完整性被破坏**：
  - 文件名含 `\n`：结果行被拆成多行，其中一行恰为 `END` → 客户端**提前结束**；残留的
    `OK 99\t…` 会成为**同一连接下一条响应**的内容（响应错位）。实测 `lsearch evil` 输出为空，
    而 `lsearch --count evil` 为 1（count2 不返回行，故正常）。
  - 文件名含 `\t`：路径字段被解析器按 tab 切分 → **路径截断、元数据错位**
    （实测 `lsearch b.txt` 打印 `…/home/ta`）。
- Linux 文件名除 `/`、NUL 外**任意字节合法**，因此 `\n`/`\t` 是合法输入——这属于**协议设计缺陷**，
  不是测试问题。

## What Changes
- **外部验证器（IPC）**：第三方工具传输（`nc -U`）+ 仅依据协议文档的独立客户端 + 与 CLI 的差分比对
- **负形状/边界矩阵（IPC）**：未知动词、缺字段、非法 sort/limit、非法 base64、超长行、无换行 EOF、
  空行、`\r\n`、非 UTF-8、批量（多请求单连接）与并发连接的响应顺序、shutdown 时序
- **帧完整性修复（产品改动）**：新增 `search3`（结果行 path 字段转义）+ 客户端能力探测与降级
- **资源边界**：单行请求上限，超限拒绝（防本地内存 DoS）
- **文档同步**：002 R5 的 "OK+END" 表述与命令清单、`proto.h` 错误目录、IPC 无 `offset` 的澄清

## Requirements

#### Requirement 1 — 第三方工具可驱动原始协议
`nc -U <sock>`（或等价第三方工具）能完成 `ping`/`capabilities`/`stats`/`search` 基本往返；
脚本断言协议为纯文本行协议、**不依赖本仓客户端库**。环境缺工具时 **SKIP**（不误报 FAIL）。

#### Requirement 2 — 独立客户端与差分测试
仅依据 `ipc/proto.h` + 本规格文本，用 Python **独立实现**客户端（禁止 import/复制仓库代码）；
对同一查询语料（子串/通配符/`re:`/排序/`under`/`-d`/`-0`/`--count`）与 `lsearch` CLI 结果做
**逐字节差分**，必须一致。实现者在编码前**不得阅读** `ipc/*.cpp`、`daemon/*.cpp`（以流程记录为证）。

#### Requirement 3 — 负形状与边界矩阵
表驱动、断言错误串与 stdout 纯净；覆盖 What Changes 所列矩阵（含 `\r\n`、无换行 EOF、空行跳过、
超长行、非 UTF-8、未知动词、非法 sort/limit、单连接多请求与并发连接的响应顺序）。

#### Requirement 4 — 帧完整性：`\n`/`\t`/`\r`/`\\` 不得破坏帧
当文件名（或 `stats`/`get-config` 的 value）含上述字节时：
- 不得产生额外行、不得伪造 `END`、不得使字段错位；
- 客户端解码后**还原原始字节**，`lsearch` 输出与真实文件名逐字节一致；
- **旧客户端连新 daemon 行为不变**（走既有 search/search2 路径）。

#### Requirement 5 — 资源边界
单行请求上限（建议 1 MiB）；超限返回 `ERR line too long` 并关闭该连接；内存不得无界增长。

#### Requirement 6 — 不回归与文档同步
既有 9 套自测与单测保持通过；002/`proto.h`/README/规格清单同步（遵循文档同步全局规则）。

## 设计（已定：`search3` + 能力协商）
- 新动词 `search3 <limit> <dirs> <files> <sort> <under64> <query...>`：响应头
  `OK <returned> <total> <capped> esc=1`，结果行 **path 字段转义**
  （`\`→`\\`、TAB→`\t`、LF→`\n`、CR→`\r`，仅 path 字段）；`count2` 不返回行，无需 v3。
- 客户端：`capabilities` 探测 `search3`（`supportsV3()`），沿用既有 `supportsV2()`/降级与
  `kUnderUnsupportedMessage` 模式；结果行解析按 v3 解码。解码集中在 `ipc/client`，各前端无需改动。
- 兼容性：旧 daemon 无 `search3` → 自动降级（字节不变）；旧客户端连新 daemon → 行为不变。
- 资源边界：单行请求上限 **1 MiB**，超限返回 `ERR line too long` 并关闭该连接。
- 已否决备选：(a) 无协商一律转义（旧客户端路径显示退化）；(b) 长度前缀 / 整行 base64
  （最健壮但与旧客户端不兼容、改动最大）。
- **范围收窄**：`stats`/`get-config` 的 value 转义**暂缓**（其值来自本地配置而非索引文件名，
  且需独立动词协商），记入偏差记录。

## 非目标
- 不改 MCP/D-Bus 表面（它们复用 `ipc/client`，解码集中一处即可）
- 不引入托管 CI

## Scenario
Given 隔离 HOME/XDG 与运行中的 `lsearchd`
When 用 `nc -U` 发送 `ping`/`capabilities`/`stats`/`search`
Then 行结构与协议文档一致，且不依赖本仓客户端代码
When 用独立客户端与 `lsearch` CLI 跑同一查询语料
Then 结果逐字节一致
When 索引中存在名为 `evil\nEND\nOK 99` 与 `ta\tb.txt` 的文件并搜索
Then 响应帧完整（无伪造 `END`、无额外行），客户端还原真实文件名，CLI 输出与真实字节一致
When 发送超过上限的单行请求
Then 返回 `ERR line too long` 并关闭连接，daemon 内存不无界增长
When 环境缺 `nc`
Then 第三方验证 SKIP 并说明原因

## Task
- [x] 探针固化：把本次实证的 `\n`/`\t` 案例写入负形状矩阵
- [x] 第三方传输脚本（`nc -U`）
- [x] 独立客户端（spec-only）+ 与 CLI 差分
- [x] `search3` + 客户端探测/降级 + 转义解码（方案已定：`search3` + 能力协商）
- [x] 请求行上限（1 MiB）
- [x] 002/`proto.h`/README/架构/规格清单同步
- [x] Evidence 填充并将状态改为 Done

## Deliverable
`scripts/self-test-ipc-external.sh`、`scripts/ipc-spec-only-client.py`、
`scripts/self-test-ipc-matrix.sh`、`ipc/` 与 `daemon/` 的 v3 支持、文档

## Evidence
（待实现后填充；当前仅有立项探针实录）

### 产品修复（R4 / R5）
- **`search3` 上线**：`capabilities` 现为 `…,search,search2,search3,count2,…`；响应
  `OK <returned> <total> <capped> esc=1` + 转义结果行 + `END`；解析与校验与 `search2`
  完全同构（相同 `ERR bad under`/`ERR bad sort`/`ERR bad regex: <msg>`）。
- **转义表单点实现**：`ipc/proto.cpp` 的 `escapeField`/`unescapeField`（daemon 编码、client
  解码共用，杜绝两处漂移）；`unescape(escape(s)) == s` 且单射（原始 `\` + `n` 不被误读为换行），
  未识别 `\x` 原样保留。
- **客户端**：惰性 `supportsV3()`（确定性结果才缓存，瞬时错误下次重试）；`searchEx()` 优先
  `search3`，实测与 capabilities 不一致（`ERR unknown command`）时降级 `search2`→legacy；
  `Client::search()`（CLI/TUI/GUI 的默认快路径）在 v3 可用时同样走 v3，使前端**零改动**即字节精确。
- **R5**：单行请求上限 1 MiB（`recv` 分片 4096 不变）；超限（含无换行的超长残行）返回
  `ERR line too long` 并关闭该连接，其他连接不受影响；恰好 1 MiB 仍接受。

修复前后对照（隔离环境，文件名含 `\n` / `\t`）：
```
# 修复前（legacy search2）：帧被击穿 —— 路径里的 \n 伪造 END，残留行污染下一条响应
OK 1 1 0
/…/home/evil
END                     ← 客户端提前结束
OK 99	0	0	…	0
END
# 修复后（search3）：单帧、仅一个 END、path 已转义
OK 1 1 0 esc=1
/…/home/evil\nEND\nOK 99	0	0	…	0
END
$ lsearch evil   → /…/home/evil\nEND\nOK 99   （od -c 证实字节精确）
$ lsearch b.txt  → /…/home/ta\tb.txt          （TAB 保留，未串列）
```

### 外部验证（R1–R3）
| 脚本 | 结果 |
|---|---|
| `scripts/self-test-ipc-external.sh -s` | 第三方 `nc -U` 直连原始协议：**7/7** |
| `scripts/self-test-ipc-diff.sh -s` | 独立客户端 × CLI 逐字节差分：**13/13** |
| `scripts/self-test-ipc-matrix.sh -s` | 负形状/边界矩阵：**33 PASS / 0 FAIL / 0 SKIP** |

- 独立客户端 `scripts/ipc-spec-only-client.py` 仅据 `ipc/proto.h` + 本规格实现
  （未读 `ipc/*.cpp`、`daemon/*.cpp`；`basedpyright` 无诊断）。
- 矩阵覆盖：未知动词、缺/多字段、非法 sort、`under` 非法/超长/控制字符、超长行、无换行 EOF、
  空行跳过、CRLF、非 UTF-8、单连接多请求顺序、两并发连接，以及 `\n`/`\t`/`\r`/`\\`
  文件名的**还原**断言。
- `LSEARCH_IPC_FORCE_NO_V3=1` 自检：v3 用例走 SKIP 分支（4 SKIP / 29 PASS），套件其余照常、退出 0。

### 回归
```
lsearch_tests                       → 511 checks, 0 failures   （原 481 + 新增 30）
self-test / ipc / mcp / dbus        → 25 / 18 / 37 / 22，全部 0 失败
mcp matrix / inspector / replay×2   → 27 / 3 / 6 / 6；conformance PASS
运行前后 pgrep -x lsearchd          → 仅真实守护进程 455627（无泄漏）
```

### 偏差记录
1. **`Client::search()` 在 v3 可用时改走 `search3`**：CLI/TUI/GUI 的默认（无 `--under`）路径
   原走 legacy `search`（早停、未排序），现经 `search3`（全扫描 + 排序后取 limit）。这是让前端
   字节精确的必要改动（缺陷 A/B 的修复），但**改变了命中数 > limit 时返回的子集**（由"任意子集"
   变为"排序后前 limit"）；`total` 仍保持 legacy 语义（=返回条数）。旧客户端二进制不受影响。
2. **`limit` 无合法性校验**（沿用既有 `atoll`→`size_t`：`-5`/`abc` 视作 0，超大值按候选上限收敛）：
   矩阵只断言**帧良构 + 守护进程存活**，不发明错误串；如需严格拒绝，应在 `proto.h` 定义规范错误。
3. **CRLF 不在协议语法内**：`ping\r\n` 的 `\r` 并入动词 → `ERR unknown command`；矩阵按
   "单行良构响应"断言，未引入 CRLF 容忍。
4. **CLI 无 `--reverse`**：反序仅 TUI F7（客户端侧）；差分中的"反序"用例为独立客户端结果与
   CLI 结果的反转比对。
5. **`stats`/`get-config` 的 value 转义暂缓**：其值来自本地配置而非索引文件名，且需独立动词协商；
   保留后续规格空间（R4 当前只覆盖结果行 path）。
6. **文档修订**：002 R5 的"所有响应 OK+END"表述与命令清单已修正；`ipc/proto.h` 补齐
   `search3`、`esc=1`、转义表与 8 条错误目录；`docs/architecture.md` 的检索路径与 IPC 外部验证
   章节同步。

### 探针实录（Spec 012 立项依据）
隔离环境 + 含 `\n`/`\t` 的文件名 + `nc -U` 直连原始协议：
```
=== query=evil（文件名 evil\nEND\nOK 99）===
OK 1
/home/code/Lsearch/.tmp/ipc-probe/home/evil
END
OK 99	0	1	1789390303	0
END
=== CLI 视角 ===
$ lsearch evil            → （空输出）
$ lsearch --count evil    → 1
=== query=b.txt（文件名 ta\tb.txt）===
/home/code/Lsearch/.tmp/ipc-probe/home/ta	b.txt	0	1	1789390303	0
$ lsearch b.txt           → /home/code/Lsearch/.tmp/ipc-probe/home/ta
```
