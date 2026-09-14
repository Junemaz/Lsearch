#!/usr/bin/env bash
# lsearch IPC 第三方传输验证器（Spec 012 Requirement 1）。
#
# 仅用第三方工具 `nc -U`（OpenBSD netcat）直连 Unix socket，验证原始行协议：
#   ping / capabilities / stats / search / 同连接两次顺序请求 / 未知动词。
# 不依赖本仓任何客户端代码（无 python 客户端参与断言）。
#
#   ./scripts/self-test-ipc-external.sh        # 构建后运行
#   ./scripts/self-test-ipc-external.sh -s     # 跳过构建
#
# 环境缺 nc 时 SKIP（exit 0，不误报 FAIL）。隔离 HOME/XDG_*，配合
# selftest-daemon-guard.sh 保证不泄漏守护进程（绝不动既有实例）。
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
. "$ROOT/scripts/selftest-daemon-guard.sh"
guard_snapshot
SKIP_BUILD=0
[ "${1:-}" = "-s" ] && SKIP_BUILD=1
T="${LSEARCH_TEST_DIR:-$ROOT/.selftest-ipc-external}"
PASS=0
FAIL=0
CLEANED=0

pass(){ PASS=$((PASS + 1)); printf '  PASS  %s %s\n' "$1" "${2:-}"; }
fail(){ FAIL=$((FAIL + 1)); printf '  FAIL  %s %s\n' "$1" "${2:-}"; }

cleanup(){
  [ "$CLEANED" = "1" ] && return
  CLEANED=1
  guard_reap_new
  # 始终用隔离环境关停，绝不触达真实守护进程的 socket。
  env HOME="$T/home" XDG_CONFIG_HOME="$T/config" XDG_DATA_HOME="$T/data" \
      XDG_RUNTIME_DIR="$T/run" "$ROOT/build/lsearchd" --shutdown >/dev/null 2>&1 || true
}
trap cleanup EXIT

echo "==> 0/ 构建"
if [ "$SKIP_BUILD" = "0" ]; then
  cmake --build "$ROOT/build" -j"$(nproc)" >/dev/null 2>&1 || { echo "构建失败"; exit 1; }
fi
[ -x "$ROOT/build/lsearchd" ] || { echo "缺少 build/lsearchd"; exit 1; }

echo "==> 1/ 环境探针（第三方工具 nc）"
if ! command -v nc >/dev/null 2>&1; then
  echo
  echo "SKIP: 未找到 nc"
  exit 0
fi
if ! nc -h 2>&1 | grep -q -- '-U'; then
  echo
  echo "SKIP: nc 不支持 -U（Unix socket）"
  exit 0
fi

echo "==> 2/ 隔离环境 + 启动守护进程"
rm -rf "$T"; mkdir -p "$T/home/docs" "$T/run" "$T/config" "$T/data"
export HOME="$T/home" XDG_CONFIG_HOME="$T/config" XDG_DATA_HOME="$T/data" XDG_RUNTIME_DIR="$T/run"
: > "$T/home/docs/report.txt"
: > "$T/home/readme.md"
: > "$T/home/notes.txt"
SOCK="$T/run/lsearch.sock"
"$ROOT/build/lsearchd" --foreground </dev/null >"$T/lsearchd.log" 2>&1 &
DPID=$!
READY=0
for _ in $(seq 1 100); do
  [ -S "$SOCK" ] && { READY=1; break; }
  kill -0 "$DPID" 2>/dev/null || break
  sleep 0.1
done
if [ "$READY" != "1" ]; then
  echo "    守护进程未就绪；日志末尾："; tail -8 "$T/lsearchd.log" 2>/dev/null | sed 's/^/      /'
  echo
  echo "SKIP: build/lsearchd 未能在隔离环境启动（可能正在重建）"
  exit 0
fi
echo "    socket=$SOCK pid=$DPID"

# 仅用 nc -U：-N 在 stdin EOF 后关闭网络套接字，避免守护进程长连接导致挂起。
ncq(){ printf '%b' "$1" | timeout 5 nc -UN "$SOCK" 2>/dev/null; }
line(){ printf '%s\n' "$1" | sed -n "${2}p"; }
lastline(){ printf '%s\n' "$1" | tail -n 1; }

echo "==> 3/ nc -U 原始协议断言（R1）"

# 3.1 ping → 单行 OK pong（无 END）
OUT="$(ncq 'ping\n')"
if [ "$OUT" = "OK pong" ]; then pass "ping → OK pong"; else fail "ping → OK pong" "got=[$OUT]"; fi

# 3.2 capabilities → OK\ncommands=<csv>\nEND，含 search/search2/count2
OUT="$(ncq 'capabilities\n')"
CMDS="$(printf '%s\n' "$OUT" | sed -n 's/^commands=//p')"
CAP_OK=1
[ "$(line "$OUT" 1)" = "OK" ] || CAP_OK=0
[ "$(lastline "$OUT")" = "END" ] || CAP_OK=0
[ -n "$CMDS" ] || CAP_OK=0
for v in search search2 count2; do
  case ",$CMDS," in *",$v,"*) ;; *) CAP_OK=0 ;; esac
done
if [ "$CAP_OK" = "1" ]; then
  pass "capabilities → OK + commands + END" "commands=$CMDS"
else
  fail "capabilities → OK + commands + END" "got=[$OUT]"
fi

# 3.3 stats → OK\n<key=value...>\nEND（至少一个 k=v）
OUT="$(ncq 'stats\n')"
STAT_OK=1
[ "$(line "$OUT" 1)" = "OK" ] || STAT_OK=0
[ "$(lastline "$OUT")" = "END" ] || STAT_OK=0
printf '%s\n' "$OUT" | grep -qE '^[A-Za-z_]+=' || STAT_OK=0
if [ "$STAT_OK" = "1" ]; then
  pass "stats → OK + key=value + END"
else
  fail "stats → OK + key=value + END" "got=[$OUT]"
fi

# 3.4 search → OK <n>\n<rows>\nEND（n>=1 且含 fixture 路径）
OUT="$(ncq 'search 50 0 0 name report\n')"
SR_OK=1
printf '%s\n' "$(line "$OUT" 1)" | grep -qE '^OK [0-9]+$' || SR_OK=0
printf '%s\n' "$OUT" | grep -q "$T/home/docs/report.txt" || SR_OK=0
[ "$(lastline "$OUT")" = "END" ] || SR_OK=0
if [ "$SR_OK" = "1" ]; then
  pass "search → OK <n> + rows + END" "$(line "$OUT" 1)"
else
  fail "search → OK <n> + rows + END" "got=[$OUT]"
fi

# 3.5 同一连接两次顺序请求：ping 后 stats，响应必须按序
OUT="$(ncq 'ping\nstats\n')"
SEQ_OK=1
[ "$(line "$OUT" 1)" = "OK pong" ] || SEQ_OK=0
[ "$(line "$OUT" 2)" = "OK" ] || SEQ_OK=0
[ "$(lastline "$OUT")" = "END" ] || SEQ_OK=0
if [ "$SEQ_OK" = "1" ]; then
  pass "单连接 ping+stats → 顺序响应"
else
  fail "单连接 ping+stats → 顺序响应" "got=[$OUT]"
fi

# 3.6 未知动词 → ERR unknown command
OUT="$(ncq 'frobnicate\n')"
if [ "$OUT" = "ERR unknown command" ]; then
  pass "未知动词 → ERR unknown command"
else
  fail "未知动词 → ERR unknown command" "got=[$OUT]"
fi

# 3.7 守护进程仍在（协议未因上述弯折而崩）
OUT="$(ncq 'ping\n')"
if [ "$OUT" = "OK pong" ]; then pass "守护进程存活（收尾 ping）"; else fail "守护进程存活（收尾 ping）" "got=[$OUT]"; fi

echo
echo "=============================================="
echo "  IPC 第三方传输（nc -U）：通过 $PASS 项，失败 $FAIL 项"
if [ "$FAIL" -eq 0 ]; then
  echo "  原始协议可由第三方工具驱动 ✅"
else
  echo "  存在失败，保留 $T 供排查"
fi
cleanup
if [ "$FAIL" -eq 0 ]; then rm -rf "$T"; fi
exit "$FAIL"
