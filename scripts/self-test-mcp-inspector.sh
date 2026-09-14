#!/usr/bin/env bash
# lsearch MCP Inspector CLI smoke test (Spec 011, Requirement 1).
#
# Third-party client check: drives `@modelcontextprotocol/inspector --cli`
# (JSON output mode) against `build/lsearch-mcp` and asserts that
#   * tools/list exposes exactly the two read-only tools
#     (search_files, index_stats), and
#   * tools/call index_stats returns a well-formed text payload with files>0.
#
#   ./scripts/self-test-mcp-inspector.sh        # build then run
#   ./scripts/self-test-mcp-inspector.sh -s     # skip build
#
# Exact inspector CLI shape:
#   npx --prefer-offline -y @modelcontextprotocol/inspector --cli <mcp> \
#       --method tools/list --format json
#   npx --prefer-offline -y @modelcontextprotocol/inspector --cli <mcp> \
#       --method tools/call --tool-name index_stats --tool-args-json '{}' --format json
#
# SKIP (exit 0) when npx/node/inspector is unavailable. Isolation: HOME/XDG_*
# under the workspace; the running real daemon/index is never touched.
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
. "$ROOT/scripts/selftest-daemon-guard.sh"
guard_snapshot
SKIP_BUILD=0
[ "${1:-}" = "-s" ] && SKIP_BUILD=1
T="${LSEARCH_TEST_DIR:-$ROOT/.selftest-mcp-inspector}"
DPID=""
CLEANED=0

skip(){
  echo
  echo "SKIP: $*"
  cleanup
  rm -rf "$T"
  exit 0
}
cleanup(){
  [ "$CLEANED" = "1" ] && return
  CLEANED=1
  guard_reap_new
  "$ROOT/build/lsearchd" --shutdown >/dev/null 2>&1 || true
  [ -n "$DPID" ] && wait "$DPID" 2>/dev/null || true
}
trap cleanup EXIT

echo "==> 0/ 构建"
if [ "$SKIP_BUILD" = "0" ]; then
  cmake --build "$ROOT/build" -j"$(nproc)" >/dev/null 2>&1 || { echo "构建失败"; exit 1; }
fi
[ -x "$ROOT/build/lsearch-mcp" ] || { echo "缺少 build/lsearch-mcp"; exit 1; }

echo "==> 1/ 环境探针（npx / node / inspector）"
command -v npx >/dev/null 2>&1 || skip "未找到 npx"
command -v node >/dev/null 2>&1 || skip "未找到 node"
# 官方 Inspector 的 engines 为 node >= 22.19.0；低版本 npm 仍会装上，但 CLI 会产出
# 空结果（rc=0），表现为「本地通过、CI 失败」。此处提前 SKIP 并说明，避免假 FAIL。
NODE_MAJOR=$(node --version 2>/dev/null | sed -n 's/^v\([0-9]*\)\..*/\1/p')
NODE_MINOR=$(node --version 2>/dev/null | sed -n 's/^v[0-9]*\.\([0-9]*\).*/\1/p')
if [ -z "$NODE_MAJOR" ] || [ "$NODE_MAJOR" -lt 22 ] || { [ "$NODE_MAJOR" -eq 22 ] && [ "$NODE_MINOR" -lt 19 ]; }; then
  skip "需要 Node >= 22.19（当前 $(node --version 2>/dev/null)）：@modelcontextprotocol/inspector 的 engines 约束"
fi
command -v python3 >/dev/null 2>&1 || skip "未找到 python3"
if ! timeout 300 npx --prefer-offline -y --package @modelcontextprotocol/inspector sh -c 'command -v mcp-inspector' >/dev/null 2>&1; then
  skip "无法解析 @modelcontextprotocol/inspector（npx 缓存缺失且无网络）"
fi
# 一次性解析绝对路径：避免每次调用都触发 npx 重新解析（离线/代理抖动会导致
# "mcp-inspector: not found" 之类的偶发失败）。解析失败一律 SKIP，不误报失败。
INSPECTOR_BIN="$(timeout 300 npx --prefer-offline -y --package @modelcontextprotocol/inspector sh -c 'command -v mcp-inspector' 2>/dev/null | tail -1)"
if [ -z "$INSPECTOR_BIN" ] || [ ! -x "$INSPECTOR_BIN" ]; then
  skip "无法解析 mcp-inspector 路径（npx 缓存缺失且无网络）"
fi
export INSPECTOR_BIN
echo "    inspector: $INSPECTOR_BIN"

echo "==> 2/ 隔离环境 + 守护进程"
rm -rf "$T"; mkdir -p "$T/home/docs" "$T/run" "$T/config" "$T/data"
export HOME="$T/home" XDG_CONFIG_HOME="$T/config" XDG_DATA_HOME="$T/data" XDG_RUNTIME_DIR="$T/run"
export ROOT MCP_STDERR="$T/mcp-stderr.txt"
echo x > "$T/home/docs/AnnualReport.txt"
echo x > "$T/home/readme.md"
"$ROOT/build/lsearchd" --foreground > "$T/daemon.log" 2>&1 &
DPID=$!
for i in $(seq 1 80); do [ -S "$T/run/lsearch.sock" ] && break; sleep 0.2; done
sleep 1
[ -S "$T/run/lsearch.sock" ] || { echo "守护进程未就绪"; exit 1; }

echo "==> 3/ Inspector CLI 冒烟"
cat > "$T/inspector.py" <<'PYEOF'
import json, os, subprocess, sys

ROOT = os.environ["ROOT"]
MCP = os.path.join(ROOT, "build", "lsearch-mcp")
HOME = os.environ["HOME"]
INSPECTOR_BIN = os.environ["INSPECTOR_BIN"]

passed = 0
failed = 0
def ok(name, msg=""):
    global passed; passed += 1; print(f"  PASS  {name} {msg}".rstrip())
def bad(name, msg=""):
    global failed; failed += 1; print(f"  FAIL  {name} {msg}".rstrip())

def inspector(extra):
    # Inspector 默认以「精简环境」启动 MCP server（只带 HOME）。显式用 -e 传 XDG_*，
    # 让 lsearch-mcp 连到本脚本的隔离 daemon，而不是落到回退路径并自动拉起新 daemon
    # —— 后者在 CI 慢机上会因新 daemon 尚未就绪而让 index_stats 返回 isError（rc=5）。
    cmd = [INSPECTOR_BIN]
    for key in ("HOME", "XDG_RUNTIME_DIR", "XDG_DATA_HOME", "XDG_CONFIG_HOME"):
        val = os.environ.get(key)
        if val:
            cmd += ["-e", f"{key}={val}"]
    cmd += ["--cli", MCP] + extra
    return subprocess.run(cmd, capture_output=True, text=True, env=os.environ)

# ---- tools/list ----
r = inspector(["--method", "tools/list", "--format", "json"])
try:
    obj = json.loads(r.stdout)
except Exception as e:
    obj = None
    bad("inspector tools/list JSON", f"rc={r.returncode} parse={e} stderr={r.stderr[-200:]}")
if obj is not None:
    tools = (obj.get("result") or {}).get("tools")
    names = sorted(t.get("name") for t in tools) if isinstance(tools, list) else None
    if r.returncode == 0 and names == ["index_stats", "search_files"]:
        ok("inspector tools/list exact 2 tools", f"tools={names}")
    else:
        bad("inspector tools/list exact 2 tools", f"rc={r.returncode} tools={names}")

# ---- tools/call index_stats ----
r = inspector(["--method", "tools/call", "--tool-name", "index_stats",
               "--tool-args-json", "{}", "--format", "json"])
payload = None
try:
    obj = json.loads(r.stdout)
    res = obj.get("result") or {}
    content = res.get("content") or []
    payload = json.loads(content[0]["text"])
    call_ok = (r.returncode == 0 and res.get("isError") is False
               and content[0].get("type") == "text")
except Exception as e:
    call_ok = False
    bad("inspector tools/call index_stats", f"rc={r.returncode} parse={e} stderr={r.stderr[-200:]}")
if call_ok:
    ok("inspector tools/call index_stats", "isError=false, text content")
if isinstance(payload, dict):
    if payload.get("files", 0) > 0 and HOME in str(payload.get("roots", "")):
        ok("inspector index_stats payload", f"files={payload.get('files')}")
    else:
        bad("inspector index_stats payload", f"{payload}")

print(f"  [inspector] passed={passed} failed={failed}")
sys.exit(1 if failed else 0)
PYEOF

OUT="$(python3 "$T/inspector.py" 2>&1)"; RC=$?
printf '%s\n' "$OUT"
PASS=$(printf '%s\n' "$OUT" | grep -c '^  PASS' || true)
FAIL=$(printf '%s\n' "$OUT" | grep -c '^  FAIL' || true)
if [ "$RC" -ne 0 ] && [ "$FAIL" -eq 0 ]; then echo "  FAIL  driver 异常退出（rc=$RC）"; FAIL=$((FAIL+1)); fi

echo
echo "=============================================="
echo "  MCP Inspector 冒烟：通过 $PASS 项，失败 $FAIL 项"
if [ "$FAIL" -eq 0 ]; then echo "  Inspector 冒烟全部通过 ✅"; else echo "  存在问题，保留 $T 供排查"; fi

cleanup
if [ "$FAIL" -eq 0 ]; then rm -rf "$T"; fi
exit "$FAIL"
