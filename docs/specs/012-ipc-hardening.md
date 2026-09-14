# Lsearch Spec 012 — IPC 协议加固（外部验证 + 帧完整性修复）

Status: **Proposed**

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

## 设计（建议，待评审）
- 新动词 `search3 <limit> <dirs> <files> <sort> <under64> <query...>`：响应头
  `OK <returned> <total> <capped> esc=1`，结果行 **path 字段转义**
  （`\\`→`\\\\`、`\t`→`\\t`、`\n`→`\\n`、`\r`→`\\r`）；`count2` 不返回行，无需 v3。
- 客户端：`capabilities` 探测扩展 v3（`search3`），沿用既有 `supportsV2()`/降级与
  `kUnderUnsupportedMessage` 模式；结果行解析按 v3 解码。
- 兼容性：旧 daemon 无 `search3` → 自动降级（字节不变）；旧客户端连新 daemon → 行为不变。
- **备选方案（待选）**：
  - (a) 无协商、对所有客户端一律转义：改动小，但旧客户端会把 `\t` 显示成两个字符；
  - (b) 长度前缀 / 整行 base64：最健壮，但改动最大且与旧客户端不兼容；
  - (c) 推荐方案 `search3` + 能力协商（上面）。

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
- [ ] 探针固化：把本次实证的 `\n`/`\t` 案例写入负形状矩阵
- [ ] 第三方传输脚本（`nc -U`）
- [ ] 独立客户端（spec-only）+ 与 CLI 差分
- [ ] `search3` + 客户端探测/降级 + 转义解码（产品改动，方案待定）
- [ ] 请求行上限
- [ ] 002/`proto.h`/README/规格清单同步
- [ ] Evidence 填充并将状态改为 Done

## Deliverable
`scripts/self-test-ipc-external.sh`、`scripts/ipc-spec-only-client.py`、
`scripts/self-test-ipc-matrix.sh`、`ipc/` 与 `daemon/` 的 v3 支持、文档

## Evidence
（待实现后填充；当前仅有立项探针实录）

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
