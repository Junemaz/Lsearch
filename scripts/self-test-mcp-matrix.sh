#!/usr/bin/env bash
# lsearch MCP negative/edge-shape matrix (Spec 011, Requirement 3).
#
# Table-driven, no timing assertions: exercises `_meta` variants, id types,
# notifications, JSON batch, method case and tools/call argument shapes, and
# asserts the expected JSON-RPC result/error code plus stdout purity (every
# emitted line must be a JSON-RPC 2.0 object).
#
#   ./scripts/self-test-mcp-matrix.sh        # build then run
#   ./scripts/self-test-mcp-matrix.sh -s     # skip build
#
# Isolation: HOME/XDG_* point under the workspace; no running daemon is touched.
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
. "$ROOT/scripts/selftest-daemon-guard.sh"
guard_snapshot
SKIP_BUILD=0
[ "${1:-}" = "-s" ] && SKIP_BUILD=1

echo "==> 0/ 构建"
if [ "$SKIP_BUILD" = "0" ]; then
  cmake --build "$ROOT/build" -j"$(nproc)" >/dev/null 2>&1 || { echo "构建失败"; exit 1; }
fi
[ -x "$ROOT/build/lsearch-mcp" ] || { echo "缺少 build/lsearch-mcp"; exit 1; }

T="${LSEARCH_TEST_DIR:-$ROOT/.selftest-mcp-matrix}"
rm -rf "$T"; mkdir -p "$T/home" "$T/run" "$T/config" "$T/data"
export HOME="$T/home" XDG_CONFIG_HOME="$T/config" XDG_DATA_HOME="$T/data" XDG_RUNTIME_DIR="$T/run"
export ROOT MCP_STDERR="$T/mcp-stderr.txt"
CLEANED=0
cleanup(){
  [ "$CLEANED" = "1" ] && return
  CLEANED=1
  guard_reap_new
  "$ROOT/build/lsearchd" --shutdown >/dev/null 2>&1 || true
}
trap cleanup EXIT

echo "==> 1/ 负形状矩阵（R3）"
cat > "$T/matrix.py" <<'PYEOF'
import json, os, subprocess, sys

ROOT = os.environ["ROOT"]
MCP = os.path.join(ROOT, "build", "lsearch-mcp")
STDERR = os.environ["MCP_STDERR"]

passed = 0
failed = 0
def ok(name, msg=""):
    global passed; passed += 1; print(f"  PASS  {name} {msg}".rstrip())
def bad(name, msg=""):
    global failed; failed += 1; print(f"  FAIL  {name} {msg}".rstrip())

err = open(STDERR, "wb")
p = subprocess.Popen([MCP], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=err)
all_lines = []
_next = [1000]
def unique_id():
    _next[0] += 1
    return _next[0]
def send(obj):
    p.stdin.write(json.dumps(obj).encode() + b"\n"); p.stdin.flush()
def send_raw(raw):
    p.stdin.write(raw.encode() + b"\n"); p.stdin.flush()
def recv():
    line = p.stdout.readline()
    if not line:
        raise RuntimeError("unexpected EOF from lsearch-mcp")
    all_lines.append(line)
    return json.loads(line.decode())

def expect_result(name, req):
    send(req)
    r = recv()
    if (isinstance(r, dict) and r.get("jsonrpc") == "2.0"
            and r.get("id") == req.get("id") and "result" in r and "error" not in r):
        ok(name, "result")
    else:
        bad(name, f"{r}")

def expect_error(name, req, code):
    send(req)
    r = recv()
    got = (r.get("error") or {}).get("code") if isinstance(r, dict) else None
    if (isinstance(r, dict) and r.get("jsonrpc") == "2.0"
            and r.get("id") == req.get("id") and "error" in r
            and got == code and isinstance((r.get("error") or {}).get("message"), str)):
        ok(name, f"error {got}")
    else:
        bad(name, f"expected error {code}, got {r}")

def expect_raw_error(name, raw, code):
    send_raw(raw)
    r = recv()
    got = (r.get("error") or {}).get("code") if isinstance(r, dict) else None
    if (isinstance(r, dict) and r.get("jsonrpc") == "2.0" and "error" in r
            and got == code and r.get("id") is None):
        ok(name, f"error {got}, id null")
    else:
        bad(name, f"expected error {code} with id null, got {r}")

def expect_noreply(name, method, params=None):
    # Deterministic "no reply" probe: send the notification, then a ping with a
    # fresh id; the NEXT line must be the ping's response (not a stray reply).
    req = {"jsonrpc": "2.0", "method": method}
    if params is not None:
        req["params"] = params
    send(req)
    pid = unique_id()
    send({"jsonrpc": "2.0", "id": pid, "method": "ping"})
    r = recv()
    if isinstance(r, dict) and r.get("id") == pid and r.get("result") == {}:
        ok(name, "no reply (next line is ping)")
    else:
        bad(name, f"notification produced a reply? got {r}")

# ---------------- _meta variants (tools/list) ----------------
def tl(meta_marker, **extra):
    params = dict(extra.pop("params", {}))
    if meta_marker is not None:
        params["_meta"] = meta_marker
    return {"jsonrpc": "2.0", "id": unique_id(), "method": "tools/list",
            "params": params}

expect_result("meta/absent -> legacy result", tl(None))
expect_result("meta/{progressToken} -> legacy result", tl({"progressToken": "tok1"}))
expect_result("meta/{} -> legacy result", tl({}))
expect_error("meta/only protocolVersion (missing clientCapabilities) -> -32602",
             tl({"io.modelcontextprotocol/protocolVersion": "2026-07-28"}), -32602)
expect_result("meta/valid modern -> result",
              tl({"io.modelcontextprotocol/protocolVersion": "2026-07-28",
                  "io.modelcontextprotocol/clientCapabilities": {}}))
expect_result("meta/non-object -> legacy result", tl(5))
expect_result("meta/unknown keys + progressToken -> legacy result",
              tl({"progressToken": "x", "io.example/unknown": 1, "foo": "bar"}))

# ---------------- id types (ping) ----------------
for label, rid in [("int", 7), ("string", "abc"), ("float", 1.5),
                   ("large int", 9007199254740991)]:
    req = {"jsonrpc": "2.0", "id": rid, "method": "ping"}
    send(req); r = recv()
    if (isinstance(r, dict) and "result" in r and r.get("result") == {}
            and r.get("id") == rid and type(r.get("id")) is type(rid)):
        ok(f"id/{label} -> result, id echoed", f"id={r.get('id')!r}")
    else:
        bad(f"id/{label} -> result, id echoed", f"{r}")

expect_noreply("id/null -> treated as notification (no reply)", "ping")

# ---------------- notifications ----------------
expect_noreply("notifications/initialized -> no reply", "notifications/initialized")
expect_noreply("notifications/cancelled -> no reply", "notifications/cancelled")

# ---------------- batch / malformed ----------------
expect_raw_error("JSON array batch -> -32600", '[{"jsonrpc":"2.0","id":1,"method":"ping"}]', -32600)
expect_raw_error("non-object JSON request -> -32600", '"hello"', -32600)
# A request object with an id but no method echoes that id back (JSON-RPC).
expect_error("missing method -> -32600",
             {"jsonrpc": "2.0", "id": unique_id()}, -32600)

# ---------------- unknown method case ----------------
expect_error("unknown method Tools/List -> -32601",
             {"jsonrpc": "2.0", "id": unique_id(), "method": "Tools/List"}, -32601)

# ---------------- tools/call argument shapes ----------------
def call(params, rid=None):
    return {"jsonrpc": "2.0", "id": rid if rid is not None else unique_id(),
            "method": "tools/call", "params": params}

expect_error("tools/call params non-object -> -32602", call([]), -32602)
expect_error("tools/call search_files missing arguments -> -32602",
             call({"name": "search_files"}), -32602)
expect_error("tools/call query wrong type -> -32602",
             call({"name": "search_files", "arguments": {"query": 5}}), -32602)
expect_error("tools/call limit wrong type -> -32602",
             call({"name": "search_files", "arguments": {"query": "x", "limit": "big"}}), -32602)
expect_error("tools/call under wrong type -> -32602",
             call({"name": "search_files", "arguments": {"query": "x", "under": 5}}), -32602)
expect_error("tools/call unknown tool -> -32602",
             call({"name": "no_such_tool"}), -32602)
expect_error("tools/call index_stats with non-empty args -> -32602",
             call({"name": "index_stats", "arguments": {"a": 1}}), -32602)

# ---------------- lifecycle + stdout purity ----------------
try:
    p.stdin.close()
    rc = p.wait(timeout=15)
    (ok if rc == 0 else bad)("lifecycle stdin-EOF exit 0", "" if rc == 0 else f"rc={rc}")
except Exception as e:
    bad("lifecycle stdin-EOF exit 0", f"exception: {e}")

pure = len(all_lines) > 0
bad_line = ""
for raw in all_lines:
    try:
        msg = json.loads(raw.decode())
        if not isinstance(msg, dict) or msg.get("jsonrpc") != "2.0":
            pure = False; bad_line = raw[:120]
    except Exception:
        pure = False; bad_line = raw[:120]
(ok if pure else bad)("stdout all lines valid JSON-RPC",
                     "" if pure else f"offending={bad_line!r}")

print(f"  [matrix] passed={passed} failed={failed}")
sys.exit(1 if failed else 0)
PYEOF

OUT="$(python3 "$T/matrix.py" 2>&1)"; RC=$?
printf '%s\n' "$OUT"
PASS=$(printf '%s\n' "$OUT" | grep -c '^  PASS' || true)
FAIL=$(printf '%s\n' "$OUT" | grep -c '^  FAIL' || true)
if [ "$RC" -ne 0 ] && [ "$FAIL" -eq 0 ]; then echo "  FAIL  driver 异常退出（rc=$RC）"; FAIL=$((FAIL+1)); fi

echo
echo "=============================================="
echo "  MCP 负形状矩阵：通过 $PASS 项，失败 $FAIL 项"
if [ "$FAIL" -eq 0 ]; then echo "  矩阵全部通过 ✅"; else echo "  存在问题，保留 $T 供排查"; fi

cleanup
if [ "$FAIL" -eq 0 ]; then rm -rf "$T"; fi
exit "$FAIL"
