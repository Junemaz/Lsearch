#!/usr/bin/env bash
# 构建 Lsearch deb 安装包
# 用法: ./packaging/build-deb.sh [arch]   (arch 默认 amd64；aarch64 需交叉工具链)
set -euo pipefail

ARCH="${1:-amd64}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BLD="$ROOT/build-pkg"
STAGE="$ROOT/packaging/stage"
OUT="$ROOT/packaging/lsearch_0.1.0-1_${ARCH}.deb"
PKGDIR="$STAGE/lsearch_0.1.0-1_${ARCH}"

echo "==> 配置并构建 (Release)..."
cmake -S "$ROOT" -B "$BLD" -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr -DBUILD_TESTS=OFF >/dev/null
cmake --build "$BLD" -j"$(nproc)" >/dev/null

echo "==> 组装软件包目录..."
rm -rf "$STAGE"
DESTDIR="$STAGE" cmake --install "$BLD" >/dev/null
if [ ! -f "$STAGE/usr/bin/lsearch-gui" ]; then
  echo "错误: 未产出 lsearch-gui（需要 Qt5 Widgets 开发包：apt install qtbase5-dev）" >&2
  exit 1
fi
mkdir -p "$PKGDIR/DEBIAN" "$PKGDIR/usr/bin" "$PKGDIR/usr/share/lsearch"
cp -f "$STAGE/usr/bin/lsearch" "$STAGE/usr/bin/lsearchd" "$STAGE/usr/bin/lsearch-tui" \
      "$STAGE/usr/bin/lsearch-gui" "$STAGE/usr/bin/lsearch-mcp" "$PKGDIR/usr/bin/"
cp -f "$STAGE/usr/share/lsearch/lsearch.conf.example" "$PKGDIR/usr/share/lsearch/"

cat > "$PKGDIR/DEBIAN/control" <<EOF
Package: lsearch
Version: 0.1.0-1
Section: utils
Priority: optional
Architecture: ${ARCH}
Depends: libc6, libsqlite3-0, libncurses6, libqt5widgets5
Maintainer: Lsearch contributors <lsearch@example.com>
Description: Everything-style filename search for Kylin Linux
 Fast filename search for 麒麟/信创桌面: a daemon (lsearchd) keeps an
 in-memory index with SQLite persistence and realtime inotify updates.
 Frontends: lsearch (CLI), lsearch-tui (TUI), lsearch-gui (Qt5) and
 lsearch-mcp (MCP stdio server for LLM agents).
EOF

echo "==> 打 deb 包..."
dpkg-deb --build --root-owner-group "$PKGDIR" "$OUT" >/dev/null
echo "完成: $OUT"
