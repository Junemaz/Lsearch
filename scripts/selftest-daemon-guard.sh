#!/usr/bin/env bash
# 自测脚本共用的守护进程防泄漏保护。
# 背景：第三方客户端（如 MCP Inspector）可能以"精简环境"启动 lsearch-mcp，
# 其自动拉起的 lsearchd 会落到回退路径（socket /tmp/lsearch-$UID、数据 $HOME/.local/share），
# 脚本自身的 cleanup 只关停"自己环境"下的实例，无法覆盖这类进程。
# 用法：source 本文件后，在拉起任何 daemon 之前调用 guard_snapshot，
#       在 cleanup 中调用 guard_reap_new（只清理运行期间新出现的 lsearchd，绝不碰既有实例）。
guard_snapshot() {
  GUARD_SNAP=$(pgrep -x lsearchd 2>/dev/null | tr '\n' ' ')
}

guard_reap_new() {
  local p
  for p in $(pgrep -x lsearchd 2>/dev/null); do
    case " $GUARD_SNAP " in *" $p "*) ;; *) kill "$p" 2>/dev/null || true ;; esac
  done
  sleep 1
  for p in $(pgrep -x lsearchd 2>/dev/null); do
    case " $GUARD_SNAP " in *" $p "*) ;; *) kill -9 "$p" 2>/dev/null || true ;; esac
  done
}
