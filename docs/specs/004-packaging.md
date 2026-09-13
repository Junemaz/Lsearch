# Lsearch Spec 004 — 打包与多架构

Status: **Done**（deb/rpm 构建均已实测） / 多架构待目标环境验证

## Why
面向麒麟 V10 等信创环境交付，需要标准安装包（rpm/deb），并覆盖 x86_64 与 aarch64
（飞腾/鲲鹏）两种信创主流架构。

## What Changes
- `packaging/build-deb.sh`：CMake 构建 → `/usr` 前缀 stage → `dpkg-deb` 出 deb
- `packaging/build-rpm.sh` + `lsearch.spec`：rpmbuild 出 rpm（需 rpm 系环境）
- 多架构：脚本以 `ARCH` 参数化；aarch64 需交叉工具链 + sysroot（本环境无，未跑）

## Requirements

#### Requirement 1 — deb 包结构正确
包含 `usr/bin/{lsearch,lsearchd,lsearch-tui}` 与 `usr/share/lsearch/lsearch.conf.example`，
`control` 声明正确 `Depends: libc6, libsqlite3-0, libncurses6`，`Architecture` 随参数。

#### Requirement 2 — rpm 脚本可用
`build-rpm.sh` 能基于 spec 产出 rpm（在装有 rpmbuild 的发行版上验证）。

#### Requirement 3 — 架构参数化
`ARCH` 参数贯穿 CMake 构建（install 前缀不变）与包元数据；aarch64 提供
工具链文件接入点（后续 CI 落地）。

## Scenario: 安装即用
When 在麒麟环境 `apt install ./lsearch_0.1.0-1_amd64.deb`（或 `rpm -ivh lsearch...rpm`）
Then `/usr/bin/lsearch`、`lsearchd`、`lsearch-tui` 就位，直接 `lsearch xxx` 可用
      （客户端会自动拉起守护进程建索引）

## Task
- [x] build-deb.sh（已在 Ubuntu 实测产出并 `dpkg-deb -I` 校验）
- [x] build-rpm.sh + lsearch.spec（Ubuntu rpmbuild 6.0.1 实测产出 rpm）
- [ ] aarch64 交叉编译工具链 + sysroot（依赖信创环境，未完成）

## Deliverable
`packaging/{build-deb.sh,build-rpm.sh,lsearch.spec}`、`packaging/lsearch_0.1.0-1_amd64.deb`

## Evidence
- 实测 `./packaging/build-deb.sh` 产出 `packaging/lsearch_0.1.0-1_amd64.deb`
- `dpkg-deb -I` 校验：Package/Version/Architecture=amd64/Depends 均正确
- `dpkg-deb -c` 校验：`/usr/bin/lsearch*` 与 `/usr/share/lsearch/*` 在包内
- rpm 实测（Ubuntu + rpmbuild 6.0.1）：`./packaging/build-rpm.sh` 产出
  `lsearch-0.1.0-1.x86_64.rpm`；`rpm -qlp` 含 5 个二进制，`rpm -qpR` 含 qt5-qtbase 等依赖
- aarch64 需交叉工具链与 sysroot（CI 落地项）
