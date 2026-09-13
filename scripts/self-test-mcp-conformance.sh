#!/usr/bin/env bash
# lsearch MCP official conformance self-test (Spec 011, Requirement 1).
#
# Drives the official MCP conformance suite (@modelcontextprotocol/conformance,
# "server" mode) against `build/lsearch-mcp`, reached through a stdio->HTTP
# bridge:
#
#   npx --prefer-offline -y supergateway@3.4.3 \
#       --stdio "<build/lsearch-mcp>" \
#       --outputTransport streamableHttp --port <free-port> --streamableHttpPath /mcp
#
# Then runs:
#
#   npx --prefer-offline -y @modelcontextprotocol/conformance server \
#       --url "http://127.0.0.1:<port>/mcp" \
#       --suite active \
#       --expected-failures scripts/mcp-conformance-baseline.yml \
#       -o <tmp>/conf-out
#
# (`--prefer-offline` is added to every npx invocation so a warm npx cache is
# used without re-resolving over a flaky network; it still falls back to the
# network on a cold cache. The generated baseline, not this script, is where
# intentionally-unsupported capabilities are declared.)
#
#   ./scripts/self-test-mcp-conformance.sh        # build then run
#   ./scripts/self-test-mcp-conformance.sh -s     # skip build
#
# SKIP (exit 0, no false FAIL) when npx/node/network is unavailable.
# On FAIL the conformance output dir and bridge log are preserved for audit.
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
. "$ROOT/scripts/selftest-daemon-guard.sh"
guard_snapshot
SKIP_BUILD=0
[ "${1:-}" = "-s" ] && SKIP_BUILD=1
BASELINE="$ROOT/scripts/mcp-conformance-baseline.yml"
T="${LSEARCH_TEST_DIR:-$ROOT/.selftest-mcp-conformance}"
BPID=""
CLEANED=0

skip(){
  echo
  echo "SKIP: $*"
  cleanup
  rm -rf "$T"
  exit 0
}
kill_tree(){
  local sig="$1" pid="$2" child
  for child in $(pgrep -P "$pid" 2>/dev/null); do kill_tree "$sig" "$child"; done
  kill "-$sig" "$pid" 2>/dev/null || true
}
cleanup(){
  [ "$CLEANED" = "1" ] && return
  CLEANED=1
  guard_reap_new
  # npx spawns supergateway (+ per-session MCP children) as descendants; killing
  # the npx wrapper alone leaks them, so signal the whole descendant tree.
  if [ -n "$BPID" ]; then
    kill_tree TERM "$BPID"
    sleep 1
    kill_tree KILL "$BPID"
    wait "$BPID" 2>/dev/null || true
  fi
  "$ROOT/build/lsearchd" --shutdown >/dev/null 2>&1 || true
}
trap cleanup EXIT

echo "==> 0/ 构建"
if [ "$SKIP_BUILD" = "0" ]; then
  cmake --build "$ROOT/build" -j"$(nproc)" >/dev/null 2>&1 || { echo "构建失败"; exit 1; }
fi
[ -x "$ROOT/build/lsearch-mcp" ] || { echo "缺少 build/lsearch-mcp"; exit 1; }
[ -f "$BASELINE" ] || { echo "缺少 conformance 基线：$BASELINE"; exit 1; }

echo "==> 1/ 环境探针（npx / node / 网络）"
command -v npx >/dev/null 2>&1 || skip "未找到 npx"
command -v node >/dev/null 2>&1 || skip "未找到 node"
command -v python3 >/dev/null 2>&1 || skip "未找到 python3"
if ! timeout 240 npx --prefer-offline -y @modelcontextprotocol/conformance --version >/dev/null 2>&1; then
  skip "无法解析 @modelcontextprotocol/conformance（npx 缓存缺失且无网络）"
fi
CONF_VER="$(timeout 240 npx --prefer-offline -y @modelcontextprotocol/conformance --version 2>/dev/null | tail -1)"
echo "    conformance=$CONF_VER"
if ! timeout 240 npx --prefer-offline -y supergateway@3.4.3 --help >/dev/null 2>&1; then
  skip "无法解析 supergateway（npx 缓存缺失且无网络）"
fi
echo "    supergateway=3.4.3"

echo "==> 2/ 隔离环境"
rm -rf "$T"; mkdir -p "$T/home/docs" "$T/run" "$T/config" "$T/data" "$T/conf-out"
export HOME="$T/home" XDG_CONFIG_HOME="$T/config" XDG_DATA_HOME="$T/data" XDG_RUNTIME_DIR="$T/run"
echo x > "$T/home/docs/AnnualReport.txt"
echo x > "$T/home/readme.md"

PORT="$(python3 - <<'PY'
import socket
s = socket.socket(); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1]); s.close()
PY
)"
echo "==> 3/ 启动 stdio→HTTP 桥（supergateway）端口 $PORT"
npx --prefer-offline -y supergateway@3.4.3 \
  --stdio "$ROOT/build/lsearch-mcp" \
  --outputTransport streamableHttp \
  --port "$PORT" --streamableHttpPath /mcp \
  </dev/null > "$T/bridge.log" 2>&1 &
BPID=$!
READY=0
for i in $(seq 1 180); do
  if timeout 1 bash -c "exec 3<>/dev/tcp/127.0.0.1/$PORT" 2>/dev/null; then READY=1; break; fi
  kill -0 "$BPID" 2>/dev/null || break
  sleep 0.5
done
if [ "$READY" != "1" ]; then
  echo "    桥未就绪；bridge.log 末尾："; tail -8 "$T/bridge.log" 2>/dev/null | sed 's/^/      /'
  skip "stdio→HTTP 桥未能启动（npx/supergateway 不可用）"
fi
echo "    桥就绪：http://127.0.0.1:$PORT/mcp"

echo "==> 4/ 官方 conformance（suite=active + baseline）"
timeout 900 npx --prefer-offline -y @modelcontextprotocol/conformance server \
  --url "http://127.0.0.1:$PORT/mcp" \
  --suite active \
  --expected-failures "$BASELINE" \
  -o "$T/conf-out" > "$T/conf.out" 2>&1
RC=$?

# If npx/network died before the suite produced results, SKIP rather than FAIL.
if grep -qE '^npm ERR!|ECONNRESET|ENOTFOUND|EAI_AGAIN' "$T/conf.out" \
   && ! grep -q 'Baseline check passed' "$T/conf.out"; then
  echo "    conformance 运行遭遇网络/npx 故障："; tail -8 "$T/conf.out" | sed 's/^/      /'
  skip "conformance 因网络/npx 不可用未完成"
fi

echo
echo "---- conformance 结果摘要 ----"
grep -E '^(Total:|✓ |✗ |\[)\s*' "$T/conf.out" || tail -20 "$T/conf.out"
echo
if grep -q 'Baseline check passed' "$T/conf.out" && [ "$RC" -eq 0 ]; then
  echo "=============================================="
  echo "  MCP conformance：PASS（所有失败均为基线内预期）✅"
  echo "  baseline: $(basename "$BASELINE")"
  cleanup
  rm -rf "$T"
  exit 0
fi

echo "=============================================="
echo "  MCP conformance：FAIL（rc=$RC，存在基线外失败或基线陈旧）"
echo "  完整输出：$T/conf.out"
echo "  结果目录：$T/conf-out（已保留）"
exit 1
