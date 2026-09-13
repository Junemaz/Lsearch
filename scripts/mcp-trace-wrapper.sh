#!/usr/bin/env bash
# lsearch MCP stdio trace wrapper (Spec 011, Requirement 2).
#
# A transparent stdio proxy that forwards client<->server JSON-RPC to
# build/lsearch-mcp unchanged, and appends every line in BOTH directions to
# $LSEARCH_MCP_TRACE as JSONL:
#
#   {"dir":"c2s","line":"<raw client -> server line>"}
#   {"dir":"s2c","line":"<raw server -> client line>"}
#
# It does not parse or alter the JSON-RPC payload (the `line` value is the raw
# line, minus the trailing newline); it only observes it.
#
# ---------------------------------------------------------------------------
# Usage: capture ONE real opencode session into a replayable trace
# ---------------------------------------------------------------------------
#   export LSEARCH_MCP_TRACE=/tmp/lsearch-mcp-real.jsonl
#   # optional: export LSEARCH_MCP_BIN=/path/to/lsearch-mcp  (defaults to ../build)
#   scripts/mcp-trace-wrapper.sh
#
# Point opencode's MCP config at the wrapper for that one session (opencode.json
# / opencode.jsonc), e.g.:
#
#   {
#     "mcp": {
#       "lsearch": {
#         "type": "local",
#         "command": ["/home/code/Lsearch/scripts/mcp-trace-wrapper.sh"],
#         "environment": { "LSEARCH_MCP_TRACE": "/tmp/lsearch-mcp-real.jsonl" }
#       }
#     }
#   }
#
# Then run a session that lists tools and calls index_stats + search_files; the
# resulting JSONL can be replayed with:
#   scripts/self-test-mcp-replay.sh /tmp/lsearch-mcp-real.jsonl
# (Add real traces under tests/fixtures/ to keep them as permanent fixtures.)
#
# Requires python3 (same dependency as the other MCP test scripts).
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
: "${LSEARCH_MCP_TRACE:?LSEARCH_MCP_TRACE must point to a writable JSONL path}"
: "${LSEARCH_MCP_BIN:=$ROOT/build/lsearch-mcp}"
export LSEARCH_MCP_BIN

exec python3 -c '
import json, os, signal, subprocess, sys, threading

trace_path = os.environ["LSEARCH_MCP_TRACE"]
mcp = os.environ["LSEARCH_MCP_BIN"]
if not os.path.exists(mcp):
    sys.stderr.write("mcp-trace-wrapper: server not found: %s\n" % mcp)
    sys.exit(2)

# Line-buffered append so a crashed/aborted session still leaves a usable trace.
trace = open(trace_path, "a", encoding="utf-8", buffering=1)
lock = threading.Lock()

def log(direction, raw):
    with lock:
        trace.write(json.dumps(
            {"dir": direction, "line": raw.decode("utf-8", "replace").rstrip("\n")},
            ensure_ascii=False) + "\n")

proc = subprocess.Popen([mcp], stdin=subprocess.PIPE, stdout=subprocess.PIPE)

def pump_client():
    try:
        for raw in sys.stdin.buffer:
            log("c2s", raw)
            try:
                proc.stdin.write(raw)
                proc.stdin.flush()
            except (BrokenPipeError, ValueError):
                break
    finally:
        try:
            proc.stdin.close()
        except Exception:
            pass

def pump_server():
    try:
        for raw in proc.stdout:
            log("s2c", raw)
            try:
                sys.stdout.buffer.write(raw)
                sys.stdout.buffer.flush()
            except BrokenPipeError:
                break
    finally:
        pass

def forward(sig, _frame):
    try:
        proc.send_signal(sig)
    except Exception:
        pass

signal.signal(signal.SIGTERM, forward)
signal.signal(signal.SIGINT, forward)

t1 = threading.Thread(target=pump_client, daemon=True)
t2 = threading.Thread(target=pump_server, daemon=True)
t1.start()
t2.start()
rc = proc.wait()
t2.join(timeout=2)
try:
    trace.close()
except Exception:
    pass
sys.exit(rc)
'
