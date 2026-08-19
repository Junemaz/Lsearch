#!/usr/bin/env bash
# 构建 Lsearch RPM 安装包
#   用法: ./packaging/build-rpm.sh [arch]
#   arch 默认 x86_64；aarch64 需提供 aarch64 工具链与 sysroot。
#   需安装 rpmbuild（rpm 系发行版自带；Ubuntu 可用 `apt install rpm`）。
set -euo pipefail

ARCH="${1:-x86_64}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BLD="$ROOT/build-pkg"
STAGE="$ROOT/packaging/stage-rpm"
TOP="$ROOT/packaging/rpmbuild"

echo "==> 配置并构建 (Release)..."
cmake -S "$ROOT" -B "$BLD" -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr -DBUILD_TESTS=OFF >/dev/null
cmake --build "$BLD" -j"$(nproc)" >/dev/null

echo "==> 组装 stage..."
rm -rf "$STAGE" "$TOP"
DESTDIR="$STAGE" cmake --install "$BLD" >/dev/null
mkdir -p "$TOP/BUILD" "$TOP/RPMS" "$TOP/SOURCES" "$TOP/SPECS" "$TOP/SRPMS"

echo "==> 打 RPM..."
rpmbuild --define "_topdir $TOP" \
         --define "ARCH $ARCH" \
         --define "buildroot $STAGE" \
         --define "_binary_payload w2.xzdio" \
         -bb "$ROOT/packaging/lsearch.spec"
find "$TOP/RPMS" -name '*.rpm' -exec cp {} "$ROOT/packaging/" \;
echo "完成: $(ls "$ROOT"/packaging/*.rpm 2>/dev/null)"
