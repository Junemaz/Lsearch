# Lsearch RPM spec（配合 packaging/build-rpm.sh 使用）
Name:           lsearch
Version:        0.1.0
Release:        1%{?dist}
Summary:        Everything-style filename search for Kylin Linux
License:        MIT
URL:            https://example.com/lsearch
BuildArch:      %{ARCH}

%description
Fast filename search for 麒麟/信创桌面: a daemon (lsearchd) keeps an in-memory
index with SQLite persistence and realtime inotify updates; lsearch (CLI) and
lsearch-tui (TUI) query it. A Qt5 GUI is planned.

%install
# %{buildroot} 由构建脚本设为 stage 目录
cp -a %{buildroot}/usr %{buildroot}/usr

%files
/usr/bin/lsearch
/usr/bin/lsearchd
/usr/bin/lsearch-tui
/usr/share/lsearch/lsearch.conf.example

%post
if command -v systemctl >/dev/null 2>&1; then
    # 预留：可为当前用户启用用户级自启服务（V2）
    :
fi
