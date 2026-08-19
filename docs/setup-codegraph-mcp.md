# 把 codegraph 接入 DSH（MCP server）

codegraph（`@colbymchenry/codegraph`，本机 `/root/.codegraph/versions/v1.5.0`）可作为
**MCP stdio server** 接入 DeepSeek Harness（DSH），使它的语义代码工具
（`codegraph_explore` 等）成为模型可直接调用的 `mcp__codegraph__*` 工具。

## 前提（本机已验证）
- codegraph 二进制：`/root/.codegraph/versions/v1.5.0/bin/codegraph`
- `codegraph serve --mcp` 可正常应答 MCP 握手（`initialize` + `tools/list` → `codegraph_explore`）
- DSH 内置 MCP 客户端桥接插件：`@deepseek-ai/dsh-mcp-client`（在 dsh 根 node_modules）

## 配置片段
追加到运行 profile 的 patch 层（web profile 为 `/root/.dsh/profiles/web/cordis.patch.yml`，
即 `cordis.patch.yml` 顶部 YAML 数组里再加一项）：

```yaml
- id: mcp-codegraph
  name: '@deepseek-ai/dsh-mcp-client'
  config:
    serverName: codegraph
    transport: stdio
    command: /root/.codegraph/versions/v1.5.0/bin/codegraph
    args: ['serve', '--mcp']
    cwd: /home/code/Lsearch
    # 可选：跟随其自身的孤儿看门狗（bin/codegraph 用 $CODEGRAPH_HOST_PPID）
    # env:
    #   CODEGRAPH_HOST_PPID: !!js process.env.PPID
```

启用后工具以 `mcp__codegraph__<工具名>` 出现（如
`mcp__codegraph__codegraph_explore`）。

## 生效要求（关键）
1. **改的是宿主配置**：`/root/.dsh/profiles/web/` 在沙箱内只读，需宿主侧有写权限；
2. **当前会话不会立刻获得工具**：MCP 工具在会话/宿主启动时注册。改完需要宿主
   **reload / HMR / 重启 GUI**，然后**新开一个会话**才会看到 `mcp__codegraph__*`；
3. 若 `failOnStartupError` 未设（默认 false），即使 server 起不来也只会"不注册工具"，
   不会搞崩 profile；但 YAML 语法错误会导致 profile 加载失败，改前先验证 YAML。

## 本仓库内的备选（无需宿主动作）
若不想动宿主，模型仍可直接用 codegraph CLI：`codegraph query/explore/node ...`，
本仓库已建好语义索引（`.codegraph/`）。
