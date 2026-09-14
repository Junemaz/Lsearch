# Lsearch Spec 013 — IPC 输入校验与配置往返完整性

Status: **Done**

## Why
本规格承接 [Spec 012](012-ipc-hardening.md) 的"非目标"（`limit` 无校验、`stats`/`get-config`
的 value 未转义）。独立复核把后者收敛为**配置往返**问题，并发现一个真实可达缺陷：

1. **`add-path` 静默损坏配置（可达缺陷）**：`daemon.cpp` 中 `add-path <p>` 把参数**原样**
   压入 `cfg_.paths`（不做 CSV 分割/校验），随后 `Config::save()` 以 `paths = a,b` 的行式
   CSV 落盘；而 `Config::load()` 用 `split(',')` + `trim` 还原。于是 `add-path /data/a,b`
   会在下次加载时变成两个根 `/data/a` 与 `/b`——**静默改变索引范围**；`get-config` 回读的
   `paths` 同样无法区分"一个含逗号路径"与"两个路径"。
2. **`stats`/`get-config` 的 value 无需转义（结论）**：其值来自**行式**配置文件
   （`load()` 逐行 `getline`），LF/CR 在配置文件中本就不可表示；IPC 请求行亦以 `\n` 分帧。
   因此 `\n` 无法进入这些 value——**可达问题是逗号歧义（见 1）**，应以输入校验解决，
   而不是新增 `stats2`/`get-config2` 动词或对 value 转义。
3. **`limit` 无校验**：`search`/`search2`/`search3` 用 `atoll` → `size_t`：`-5` 变巨值、
   `abc` 变 0（等价候选上限），畸形输入被静默接受，客户端无法得知自己发错了。
4. **`remove-path` 不落盘（可达缺陷）**：`daemon.cpp` 中 `remove-path` 会从内存配置与 DB
   移除子树并重启 watcher，**但没有调用 `Config::save()`**（全文件仅在 add-path / set-paths /
   set-excludes / set-opts 四处保存）——守护进程重启后该路径**复活**。

## What Changes
- **R1 路径参数校验**：拒绝无法在 CSV 配置中忠实往返的路径（含逗号/控制字符/首尾空白/空/超长）
- **R1b `remove-path` 持久化**：移除后必须落盘（修 4 的缺陷）
- **R2 `limit` 严格校验**：非负十进制且有上限，违规返回新错误
- **R3 测试**：IPC 矩阵新增校验用例、单测覆盖校验函数、独立客户端差分保持全绿
- **R4 文档**：`proto.h` 错误目录、002 命令/错误说明、012 非目标收口、README/规格清单

## Requirements

#### Requirement 1 — 路径参数校验（原子：先校验后写入）
`add-path` / `remove-path` 的路径参数，以及 `set-paths` / `set-excludes` 的**每个 CSV 元素**，
必须满足：非空、不含 `,`、不含控制字符（`<0x20` 与 `0x7f`）、无首尾空格/TAB、长度 ≤ 4096。
任一违规 → 返回新错误 **`ERR bad path`**，且**不得修改内存配置、不得落盘、不得触发重建**。
合法输入的行为与响应字节保持不变。`set-excludes` 的 `_`（清空哨兵）仅在**整体参数**等于
`_` 时有效，不参与元素校验。

**R1b — `remove-path` 的例外与持久化**：`remove-path` 的参数只做**非空**校验（不做
逗号/控制字符检查），以便能**精确移除历史上误加入的含逗号条目**（修复路径）；但它
**必须调用 `Config::save()` 落盘**（当前实现遗漏，见 Why 第 4 条）。

#### Requirement 2 — `limit` 严格校验
`search` / `search2` / `search3` 的 `limit` 必须为**纯十进制 ASCII 数字**（非空、无符号、
无前导 `+`）且 `0 ≤ limit ≤ 1048576`；否则返回新错误 **`ERR bad limit`**。
校验顺序固定为：`splitHead`（缺字段 → 既有错误）→ `limit` → `sort` → `under` → `query`。
合法输入的响应与之前逐字节一致（`0` 仍表示不限制）。前导零（如 `0200`）按十进制接受。

#### Requirement 3 — 测试与不回归
- `scripts/self-test-ipc-matrix.sh` 增加：`add-path` 含逗号/控制字符 → `ERR bad path`；合法
  `add-path` 往返一致；`get-config` 在拒绝后未变化；`search*` 非法 `limit` → `ERR bad limit`；
  合法 `limit` 响应不变。
- 单测覆盖路径/limit 校验函数（含边界：空、`a,b`、含 `\t`、4096/4097、`0`/`1048576`/`1048577`、
  `-1`、`abc`、前导 `+`）。
- 既有 9 套自测 + `self-test-ipc-*` 3 套与 `lsearch_tests` 保持全绿；独立客户端 × CLI 差分 13/13。

#### Requirement 4 — 文档同步
- `ipc/proto.h` 错误目录补 `ERR bad path`、`ERR bad limit`，并注明 `add-path` 不做 CSV 分割。
- `docs/specs/002-search-ipc.md` 的 R5 命令/错误说明同步。
- `docs/specs/012-ipc-hardening.md` 偏差记录补一条：value 转义**不需要**，改为本规格的输入校验；
- README（自测/协议说明）与规格清单同步（遵循文档同步全局规则）。

## Scenario
Given 隔离 HOME/XDG 与运行中的 `lsearchd`
When 发送 `add-path /data/a,b`（或含 `\t`、空、超长）
Then 返回 `ERR bad path`，`get-config` 的 `paths` 不变，且配置文件不新增行、不触发重建
When 发送 `add-path /data/ok`
Then 返回 `OK`，`get-config` 往返得到完全相同的单个路径
When 发送 `search2 abc 0 0 name - x`（或 `-1`、`1048577`）
Then 返回 `ERR bad limit`
When 发送 `search2 200 0 0 name - x`
Then 响应与升级前逐字节一致
When 发送 `add-path /data/ok` 后再 `remove-path /data/ok`
Then `get-config` 不再包含该路径，**且配置文件已落盘**（重启后不复活）
When 独立客户端与 CLI 跑同一语料
Then 逐字节一致（13/13）

## 非目标
- **不改配置文件的格式**（不引入引号/转义编码）：采用"拒绝无法表示的输入"（R1）。
  若将来要支持**含逗号**的路径，则需要配置文件格式升级与迁移，另开规格。
- 不新增 `stats2`/`get-config2` 动词、不对 kv value 转义（论证见 Why 第 2 条）。
- 不引入托管 CI（另见后续规格）。

## Task
- [x] 校验函数（路径/limit）放入 `core/util`（可单测），daemon 接入（先校验后写入）
- [x] `add-path`/`set-paths`/`set-excludes` 拒绝非法路径；`remove-path` 非空 + 落盘
- [x] `limit` 严格校验（search/search2/search3）
- [x] `proto.h` / 002 / 012 / README / 规格清单同步
- [x] 单测 + IPC 矩阵用例；全量复跑
- [x] Evidence 填充并转 Done

## 设计（已定）
- R1 采用**拒绝不可表示的输入**（不改配置文件格式、不引入引号/转义编码）；含逗号路径的
  支持需要配置格式升级与迁移，属后续规格。
- 校验函数放在 `core/util.{h,cpp}`（daemon 与单测共用，避免逻辑分叉）：
  `validConfigPath(const std::string&, std::string& why)` 与
  `parseLimit(const std::string&, size_t& out)`。

## Deliverable
`daemon/`、`ipc/`（校验 + 错误串）、单测、`scripts/self-test-ipc-matrix.sh` 新增用例、文档

## Evidence

### 产品修复（R1 / R1b / R2）
- `core/util.{h,cpp}`：`validConfigPath`（非空、无 `,`、无控制字节 `<0x20`/`0x7f`、
  无首尾空格、≤ `kConfigPathMax`=4096）与 `parseLimit`（纯十进制 ASCII、`≤ kLimitMax`=1048576、
  前导零按十进制）；均为可单测的公共函数。
- `daemon/daemon.cpp`：`search`/`search2`/`search3` 在 `splitHead` 之后、`sort`/`under`/`query`
  之前校验 `limit` → `ERR bad limit`；`add-path`/`set-paths`/`set-excludes` 逐元素校验 →
  `ERR bad path` 且**零副作用**（不改内存配置、不落盘、不重建）；`remove-path` 仅非空校验，
  并在移除后**补上 `Config::save()`**（R1b）。
- 合法输入逐字节不变（`0` 仍=不限制；`0200`→200）；`set-paths` 整体为空仍是
  `ERR paths must not be empty`；`set-excludes _` 仍清空。

### 实测（隔离 HOME/XDG + `nc -U` 直连）
```
add-path /data/a,b                → ERR bad path ；lsearch.conf md5 不变（e29a… → e29a…）
add-path <含 TAB> / 空参数         → ERR bad path
add-path <合法目录>                → OK ；get-config 单元素 ；配置落盘一次
remove-path <该目录>               → OK ；get-config 不再含 ；lsearch.conf 不再含
search / search2 / search3 abc|-1|1048577 → ERR bad limit
search2 1048576 → OK（边界接受）  ；search2 1048577 → ERR bad limit
search2 200 / 0200               → OK <r> <t> <c>（形状不变）
搜索 18446744073709551617 / 10^21 → ERR bad limit（逐位比较天然避免 size_t 回绕）
```

### 测试
| 项 | 结果 |
|---|---|
| `lsearch_tests` | **552 checks / 0 failures**（+41，新增 `tests/test_validate.cpp`） |
| `self-test-ipc-matrix.sh -s` | **61 / 0 / 0**（原 33 + 28：limit 11、路径拒绝 9、合法往返 3、落盘 3、回归 2） |
| `self-test-ipc-external.sh -s` / `self-test-ipc-diff.sh -s` | 7/7、13/13（不回归） |
| 矩阵版本门 | 未实现时新用例走 `SKIP(v13)`（33 PASS / 18 SKIP / 0 FAIL），套件不误报 |

### 回归
`self-test` 25、`self-test-ipc` 18、`self-test-mcp` 37、`self-test-dbus` 22、
`self-test-mcp-matrix` 27、`inspector` 3、`replay` 6+6、`conformance` PASS；运行前后
`pgrep -x lsearchd` 仅真实守护进程 455627（无泄漏）。

### 偏差记录
1. `remove-path` 仅校验非空（不做逗号/控制字符检查），以便精确移除历史误加入的含逗号条目；
   其余**写入型**动词（`add-path`/`set-paths`/`set-excludes`）严格校验。
2. `add-path` / `remove-path` 的**空参数**由既有 `ERR unknown command` 改为 `ERR bad path`
   （显式化；无既有测试依赖旧行为）。
3. `set-paths` 整体为空保留既有 `ERR paths must not be empty`（不改为 `ERR bad path`）。
4. **含逗号路径的支持**（配置文件格式升级 + 迁移）列为后续规格；本规格只做拒绝。
5. 无新增动词、不对 kv value 转义（论证见 Why 第 2 条；012 的相应非目标据此收口）。
