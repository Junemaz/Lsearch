#!/usr/bin/env bash
# lsearch MCP trace replay self-test (Spec 011, Requirement 2).
#
# Replays the client->server lines of a JSONL trace (as produced by
# scripts/mcp-trace-wrapper.sh) into build/lsearch-mcp and asserts that every
# response is a valid JSON-RPC message whose structure matches the recorded
# server->client response for the same (normalized) id.
#
#   ./scripts/self-test-mcp-replay.sh [-s] [trace.jsonl]
#
#   -s           skip the build step (use the existing build/)
#   trace.jsonl  trace to replay; defaults to
#                tests/fixtures/mcp-trace-synthetic.jsonl
#
# Structural equivalence only: scalars (paths, counts, mtimes, ids) are
# normalized to their JSON type, list lengths are not compared, and no
# timestamp/ordering-dependent value is asserted.
#
# The default fixture is synthetic (generated with the wrapper against an
# isolated daemon). A REAL opencode trace is captured by the user in one
# session (see scripts/mcp-trace-wrapper.sh) and can be added under
# tests/fixtures/ and replayed with the same command.
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
. "$ROOT/scripts/selftest-daemon-guard.sh"
guard_snapshot
SKIP_BUILD=0
TRACE=""
for a in "$@"; do
  case "$a" in
    -s) SKIP_BUILD=1 ;;
    *)  TRACE="$a" ;;
  esac
done
[ -n "$TRACE" ] || TRACE="$ROOT/tests/fixtures/mcp-trace-synthetic.jsonl"
if [ ! -f "$TRACE" ]; then echo "缺少 trace 文件：$TRACE"; exit 1; fi

echo "==> 0/ 构建"
if [ "$SKIP_BUILD" = "0" ]; then
  cmake --build "$ROOT/build" -j"$(nproc)" >/dev/null 2>&1 || { echo "构建失败"; exit 1; }
fi
[ -x "$ROOT/build/lsearch-mcp" ] || { echo "缺少 build/lsearch-mcp"; exit 1; }

T="${LSEARCH_TEST_DIR:-$ROOT/.selftest-mcp-replay}"
rm -rf "$T"; mkdir -p "$T/home/docs" "$T/run" "$T/config" "$T/data"
export HOME="$T/home" XDG_CONFIG_HOME="$T/config" XDG_DATA_HOME="$T/data" XDG_RUNTIME_DIR="$T/run"
export ROOT TRACE MCP_STDERR="$T/mcp-stderr.txt"
echo x > "$T/home/docs/AnnualReport.txt"
echo x > "$T/home/readme.md"

echo "==> 1/ 隔离环境 + 守护进程"
"$ROOT/build/lsearchd" --foreground > "$T/daemon.log" 2>&1 &
DPID=$!
CLEANED=0
cleanup(){
  [ "$CLEANED" = "1" ] && return
  CLEANED=1
  guard_reap_new
  "$ROOT/build/lsearchd" --shutdown >/dev/null 2>&1 || true
  wait "$DPID" 2>/dev/null || true
}
trap cleanup EXIT
for i in $(seq 1 80); do [ -S "$T/run/lsearch.sock" ] && break; sleep 0.2; done
sleep 1
[ -S "$T/run/lsearch.sock" ] || { echo "守护进程未就绪"; exit 1; }

echo "==> 2/ 回放 $(basename "$TRACE")"
cat > "$T/replay.py" <<'PYEOF'
import json, os, subprocess, sys

ROOT = os.environ["ROOT"]
MCP = os.path.join(ROOT, "build", "lsearch-mcp")
TRACE = os.environ["TRACE"]
STDERR = os.environ["MCP_STDERR"]

passed = 0
failed = 0
def ok(name, msg=""):
    global passed; passed += 1; print(f"  PASS  {name} {msg}".rstrip())
def bad(name, msg=""):
    global failed; failed += 1; print(f"  FAIL  {name} {msg}".rstrip())

# ---- load fixture: split directions, index recorded responses by id ----
entries = []
with open(TRACE, encoding="utf-8") as f:
    for raw in f:
        raw = raw.strip()
        if not raw:
            continue
        entries.append(json.loads(raw))
requests = [e for e in entries if e.get("dir") == "c2s"]
recorded = {}
for e in entries:
    if e.get("dir") != "s2c":
        continue
    try:
        msg = json.loads(e["line"])
    except Exception:
        continue
    if isinstance(msg, dict) and msg.get("id") is not None:
        recorded[str(msg["id"])] = msg

def shape(v):
    """JSON type structure; scalars collapse to their type, lists collapse to
    the shape of their first element, volatile keys are dropped."""
    if isinstance(v, dict):
        return {k: shape(val) for k, val in sorted(v.items()) if k != "id"}
    if isinstance(v, list):
        return [shape(v[0])] if v else []
    if isinstance(v, bool):
        return "bool"
    if isinstance(v, (int, float)):
        return "number"
    if v is None:
        return "null"
    return "string"

def shape_match(a, b):
    """递归比较 shape() 结果。空数组与任意数组视为结构兼容：回放跑在隔离空索引上，
    结果集基数（如 search_files.results 条数）由数据决定，不构成协议差异；
    非空时仍逐元素比较元素结构。"""
    if isinstance(a, dict) and isinstance(b, dict):
        return a.keys() == b.keys() and all(shape_match(a[k], b[k]) for k in a)
    if isinstance(a, list) and isinstance(b, list):
        if not a or not b:
            return True
        return shape_match(a[0], b[0])
    return a == b

def text_payload(msg):
    """For tools/call results, parse content[0].text as JSON."""
    try:
        return json.loads(msg["result"]["content"][0]["text"])
    except Exception:
        return None

class Client:
    def __init__(self):
        self.err = open(STDERR, "wb")
        self.p = subprocess.Popen([MCP], stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE, stderr=self.err)
        self.lines = []
    def send_raw(self, raw):
        self.p.stdin.write(raw.encode() + b"\n")
        self.p.stdin.flush()
    def recv(self):
        line = self.p.stdout.readline()
        if not line:
            raise RuntimeError("unexpected EOF from lsearch-mcp")
        self.lines.append(line)
        return json.loads(line.decode())

c = Client()

# ---- replay each client->server line in order ----
for idx, req_entry in enumerate(requests):
    raw = req_entry["line"]
    try:
        obj = json.loads(raw)
    except Exception as e:
        bad(f"replay[{idx}] parseable request", f"{e}")
        continue
    method = obj.get("method", "?")
    rid = obj.get("id", None)
    is_request = rid is not None and not isinstance(obj, list)
    label = f"replay[{idx}] {method} id={rid!r}"

    try:
        c.send_raw(raw)
        if not is_request:
            continue  # notification: no reply asserted here
        resp = c.recv()

        # 1) JSON-RPC envelope + id echo (normalized: JSON numbers compare equal)
        env_ok = (isinstance(resp, dict) and resp.get("jsonrpc") == "2.0"
                  and resp.get("id") == rid
                  and ("result" in resp) != ("error" in resp))
        if not env_ok:
            bad(label, f"bad JSON-RPC envelope: {resp}")
            continue

        # 2) per-method result/error shape
        method_ok = True
        note = ""
        if method == "initialize":
            res = resp.get("result") or {}
            method_ok = (isinstance(res.get("protocolVersion"), str)
                         and isinstance(res.get("capabilities"), dict)
                         and isinstance(res.get("serverInfo"), dict))
            note = "initialize result keys"
        elif method == "tools/list":
            tools = (resp.get("result") or {}).get("tools")
            names = sorted(t.get("name") for t in tools) if isinstance(tools, list) else []
            method_ok = names == ["index_stats", "search_files"]
            note = f"tools={names}"
        elif method == "tools/call":
            payload = text_payload(resp)
            tool = (obj.get("params") or {}).get("name")
            if tool == "index_stats":
                method_ok = isinstance(payload, dict) and {
                    "roots", "files", "dirs", "size", "rebuilding",
                    "scan_files", "scan_dirs", "uptime", "version"} <= set(payload)
                note = "index_stats payload keys"
            elif tool == "search_files":
                method_ok = (isinstance(payload, dict)
                             and isinstance(payload.get("results"), list)
                             and {"offset", "limit", "returned", "has_more", "truncated",
                                  "total", "total_capped", "total_is_lower_bound",
                                  "hint", "rebuilding"} <= set(payload.get("page") or {}))
                note = "search_files payload keys"
            else:
                method_ok = False
                note = f"unknown tool {tool!r}"
        else:
            note = "method shape not specialized"

        # 3) structural equivalence vs the recorded response
        exp = recorded.get(str(rid))
        struct_ok = exp is not None and shape_match(shape(resp), shape(exp))
        if exp is not None and method == "tools/call":
            struct_ok = struct_ok and shape_match(shape(text_payload(resp)), shape(text_payload(exp)))
        if exp is None:
            struct_ok = False
            note = (note + "; no recorded response for id").strip("; ")

        if method_ok and struct_ok:
            ok(label, note)
        else:
            bad(label, f"{note}; recorded_match={struct_ok} resp={json.dumps(resp)[:200]}")
    except Exception as e:
        bad(label, f"exception: {e}")

# ---- lifecycle: stdin EOF -> exit 0 ----
try:
    c.p.stdin.close()
    rc = c.p.wait(timeout=15)
    if rc == 0:
        ok("replay lifecycle stdin-EOF exit 0")
    else:
        bad("replay lifecycle stdin-EOF exit 0", f"rc={rc}")
except Exception as e:
    bad("replay lifecycle stdin-EOF exit 0", f"exception: {e}")

# ---- stdout purity: every emitted line is valid JSON-RPC ----
try:
    pure = len(c.lines) > 0
    for raw in c.lines:
        try:
            msg = json.loads(raw.decode())
            if not isinstance(msg, dict) or msg.get("jsonrpc") != "2.0":
                pure = False
        except Exception:
            pure = False
    (ok if pure else bad)("replay stdout all JSON-RPC",
                         "" if pure else f"lines={len(c.lines)}")
except Exception as e:
    bad("replay stdout all JSON-RPC", f"exception: {e}")

print(f"  [replay] passed={passed} failed={failed}")
sys.exit(1 if failed else 0)
PYEOF

OUT="$(python3 "$T/replay.py" 2>&1)"; RC=$?
printf '%s\n' "$OUT"
PASS=$(printf '%s\n' "$OUT" | grep -c '^  PASS' || true)
FAIL=$(printf '%s\n' "$OUT" | grep -c '^  FAIL' || true)
if [ "$RC" -ne 0 ] && [ "$FAIL" -eq 0 ]; then echo "  FAIL  driver 异常退出（rc=$RC）"; FAIL=$((FAIL+1)); fi

echo
echo "=============================================="
echo "  MCP 转录回放：通过 $PASS 项，失败 $FAIL 项（trace=$(basename "$TRACE")）"
if [ "$FAIL" -eq 0 ]; then echo "  转录回放全部通过 ✅"; else echo "  存在问题，保留 $T 供排查"; fi

cleanup
if [ "$FAIL" -eq 0 ]; then rm -rf "$T"; fi
exit "$FAIL"
