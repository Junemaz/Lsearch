# Lsearch RPM spec（配合 packaging/build-rpm.sh 使用）
Name:           lsearch
Version:        0.1.0
Release:        1%{?dist}
Summary:        Everything-style filename search for Kylin Linux
License:        MIT
URL:            https://example.com/lsearch
BuildArch:      %{ARCH}
Requires:       sqlite-libs
Requires:       ncurses-libs
Requires:       qt5-qtbase

%description
Fast filename search for 麒麟/信创桌面: a daemon (lsearchd) keeps an in-memory
index with SQLite persistence and realtime inotify updates.
Frontends: lsearch (CLI), lsearch-tui (TUI), lsearch-gui (Qt5) and
lsearch-mcp (MCP stdio server for LLM agents).

%prep
# 二进制打包：无源码；%{buildroot} 由 build-rpm.sh 以 DESTDIR 预填充

%build

%install
# stage 由 build-rpm.sh 以 DESTDIR 预填充，这里拷入 rpmbuild 管理的 %{buildroot}
mkdir -p %{buildroot}
cp -a %{stage_dir}/usr %{buildroot}/

%files
/usr/bin/lsearch
/usr/bin/lsearchd
/usr/bin/lsearch-tui
/usr/bin/lsearch-gui
/usr/bin/lsearch-mcp
/usr/share/lsearch/lsearch.conf.example

%post
if command -v systemctl >/dev/null 2>&1; then
    # 预留：可为当前用户启用用户级自启服务（V2）
    :
fi
