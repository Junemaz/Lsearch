#!/usr/bin/env bash
# Lsearch 核心流程自测脚本
#   ./scripts/self-test.sh            # 自动构建后跑全部项
#   ./scripts/self-test.sh -s         # 跳过重新构建（使用已有 build/）
# 覆盖：构建+单测 / 建索引 / 搜索(子串·通配符·计数·排序·仅目录) /
#       inotify 增量(新建·删除) / 停机期间新增+--rebuild / stats / 关闭清理
# 说明：守护进程与客户端须在同一进程环境运行，故全部在此脚本内完成。
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SKIP_BUILD=0
[ "${1:-}" = "-s" ] && SKIP_BUILD=1
T="${LSEARCH_TEST_DIR:-$ROOT/.selftest}"
PASS=0; FAIL=0
ok(){ PASS=$((PASS+1)); echo "  PASS  $1"; }
bad(){ FAIL=$((FAIL+1)); echo "  FAIL  $1"; }

echo "==> 0/ 构建与单元测试"
if [ "$SKIP_BUILD" = "0" ]; then
  cmake --build "$ROOT/build" -j"$(nproc)" >/dev/null 2>&1 || { echo "构建失败，请先: cmake -S . -B build -DCMAKE_BUILD_TYPE=Release"; exit 1; }
fi
"$ROOT/build/lsearch_tests" >/dev/null 2>&1 && ok "单元测试（lsearch_tests）全部通过" || bad "单元测试有失败"

echo "==> 1/ 准备隔离测试环境（$T）"
rm -rf "$T"
mkdir -p "$T/home/docs" "$T/home/pics" "$T/run"
export HOME="$T/home" XDG_CONFIG_HOME="$T/config" XDG_DATA_HOME="$T/data" XDG_RUNTIME_DIR="$T/run"
echo x > "$T/home/docs/AnnualReport.txt"
echo x > "$T/home/pics/vacation_photo.jpg"
echo x > "$T/home/readme.md"
mkdir -p "$T/home/sub/deeper"; echo x > "$T/home/sub/deeper/archive.tar.gz"

echo "==> 2/ 启动守护进程并建索引"
"$ROOT/build/lsearchd" --foreground > "$T/log.txt" 2>&1 &
DPID=$!
for i in $(seq 1 80); do [ -S "$T/run/lsearch.sock" ] && break; sleep 0.2; done
sleep 1
[ -S "$T/run/lsearch.sock" ] && ok "守护进程就绪（socket 已建）" || bad "守护进程未就绪"
grep -q "scan done" "$T/log.txt" && ok "全量建索引完成" || bad "全量建索引日志缺失"

echo "==> 3/ 核心搜索自测"
L="$ROOT/build/lsearch"
if "$L" -m report 2>/dev/null | grep -q "AnnualReport.txt"; then ok "子串搜索 'report' → AnnualReport.txt"; else bad "子串搜索"; fi
if "$L" -m '*.jpg' 2>/dev/null | grep -q "vacation_photo.jpg"; then ok "通配符 '*.jpg' → vacation_photo.jpg"; else bad "通配符"; fi
[ "$("$L" -m --count report 2>/dev/null)" = "1" ] && ok "计数 'report' = 1" || bad "计数 report"
[ "$("$L" -m --count 'vacation' 2>/dev/null)" = "1" ] && ok "计数 'vacation' = 1" || bad "计数 vacation"
if "$L" -m -d -S deeper 2>/dev/null | head -1 | grep -q "	目录"; then ok "仅目录 -d 'deeper' → 类型列=目录"; else bad "仅目录过滤"; fi
if "$L" -m --sort size -S deeper 2>/dev/null | head -1 | grep -q "	目录"; then ok "按大小排序首行=目录"; else bad "按大小排序"; fi
"$L" -m __nonexistent_xyz__ >/dev/null 2>&1
[ $? -eq 1 ] && ok "无命中退出码 = 1（脚本友好）" || bad "无命中退出码"

echo "==> 4/ inotify 实时增量自测"
echo new > "$T/home/docs/BrandNewDoc.pdf"; sleep 1
"$L" -m BrandNewDoc 2>/dev/null | grep -q "BrandNewDoc" && ok "新建文件被 inotify 检出" || bad "新建检出"
rm "$T/home/docs/BrandNewDoc.pdf"; sleep 1
"$L" -m BrandNewDoc >/dev/null 2>&1
[ $? -eq 1 ] && ok "删除后 inotify 移除成功" || bad "删除移除"

echo "==> 5/ 停机期间新增 + --rebuild 补齐"
"$ROOT/build/lsearchd" --shutdown >/dev/null 2>&1; sleep 1; wait $DPID 2>/dev/null
echo x > "$T/home/docs/offline_note.txt"   # 停机期间写入，inotify 不生效
"$ROOT/build/lsearchd" --rebuild --foreground >> "$T/log.txt" 2>&1 &
DPID=$!
for i in $(seq 1 80); do [ -S "$T/run/lsearch.sock" ] && break; sleep 0.2; done; sleep 1.5
"$L" -m offline_note 2>/dev/null | grep -q "offline_note" && ok "--rebuild 检出停机期间新增文件" || bad "--rebuild 未检出"

echo "==> 6/ 统计与关闭"
"$L" -m --stats 2>/dev/null | grep -q "^files=" && ok "stats 可用" || bad "stats"
"$ROOT/build/lsearchd" --shutdown >/dev/null 2>&1; sleep 1; wait $DPID 2>/dev/null
[ -S "$T/run/lsearch.sock" ] && bad "shutdown 后 socket 未清理" || ok "shutdown 后 socket 已清理"

rm -rf "$T"
echo
echo "=============================================="
echo "  通过 $PASS 项，失败 $FAIL 项"
if [ "$FAIL" -eq 0 ]; then echo "  核心流程全部通过 ✅"; else echo "  存在问题，请核对上方 FAIL 项"; fi
exit "$FAIL"
