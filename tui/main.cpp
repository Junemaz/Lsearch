// Lsearch TUI：ncurses 交互界面（输入即搜、方向键浏览、Enter 打开、F5 重建）
#include "core/config.h"
#include "core/entry.h"
#include "core/util.h"
#include "ipc/client.h"

#if defined(TUI_USE_NCURSESW)
#include <ncursesw/curses.h>
#else
#include <curses.h>
#endif

#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <clocale>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace lsearch;

namespace {

std::string g_query;
std::mutex g_qmut;
std::atomic<bool> g_needSearch{true};
std::atomic<bool> g_run{true};

std::mutex g_rmut;
std::vector<SearchResult> g_results;
std::atomic<size_t> g_total{0};
int g_cursor = 0;  // 选中行
int g_scroll = 0;  // 滚动偏移

std::string g_status;
std::mutex g_statusM;

std::string wideToUtf8(wint_t wc) {
  std::string out;
  char buf[8];
  if (wc <= 0x7F) {
    out.push_back(static_cast<char>(wc));
  } else if (wc <= 0x7FF) {
    buf[0] = char(0xC0 | (wc >> 6));
    buf[1] = char(0x80 | (wc & 0x3F));
    out.append(buf, 2);
  } else if (wc <= 0xFFFF) {
    buf[0] = char(0xE0 | (wc >> 12));
    buf[1] = char(0x80 | ((wc >> 6) & 0x3F));
    buf[2] = char(0x80 | (wc & 0x3F));
    out.append(buf, 3);
  } else {
    buf[0] = char(0xF0 | (wc >> 18));
    buf[1] = char(0x80 | ((wc >> 12) & 0x3F));
    buf[2] = char(0x80 | ((wc >> 6) & 0x3F));
    buf[3] = char(0x80 | (wc & 0x3F));
    out.append(buf, 4);
  }
  return out;
}

void setStatus(const std::string& s) {
  std::lock_guard<std::mutex> lk(g_statusM);
  g_status = s;
}

void openSelected() {
  std::string path;
  {
    std::lock_guard<std::mutex> lk(g_rmut);
    if (g_cursor >= 0 && g_cursor < static_cast<int>(g_results.size()))
      path = g_results[static_cast<size_t>(g_cursor)].entry.path;
  }
  if (path.empty()) return;
  pid_t pid = fork();
  if (pid == 0) {
    execlp("xdg-open", "xdg-open", path.c_str(), (char*)nullptr);
    _exit(127);
  }
  if (pid > 0) {
    int st;
    waitpid(pid, &st, 0);
  }
}

void triggerRebuild() {
  std::thread([] {
    Client c;
    std::string e;
    if (Client::connectOrSpawn(Config::load("").sock_path, true, c, e))
      c.command("rebuild", e);
  }).detach();
}

void worker() {
  Client c;
  std::string err;
  if (!Client::connectOrSpawn(Config::load("").sock_path, true, c, err)) {
    setStatus("无法连接守护进程: " + err);
    return;
  }
  int64_t lastStats = 0;
  while (g_run) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    int64_t now = nowSeconds();
    if (now - lastStats >= 3) {
      lastStats = now;
      std::vector<std::pair<std::string, std::string>> kv;
      if (c.stats(kv, err)) {
        std::string files, dirs, size;
        for (auto& [k, v] : kv) {
          if (k == "files") files = v;
          else if (k == "dirs") dirs = v;
          else if (k == "size") size = humanSize(atoll(v.c_str()));
        }
        setStatus("索引 files=" + files + " dirs=" + dirs + " size=" + size);
      }
    }

    if (!g_needSearch.exchange(false)) continue;

    std::string q;
    { std::lock_guard<std::mutex> lk(g_qmut); q = g_query; }

    if (q.empty()) {
      std::lock_guard<std::mutex> lk(g_rmut);
      g_results.clear();
      g_total = 0;
      g_cursor = 0;
      g_scroll = 0;
      continue;
    }

    std::vector<SearchResult> res;
    size_t total = 0;
    if (c.search(q, SortKey::Name, 2000, false, false, res, &total, err)) {
      std::lock_guard<std::mutex> lk(g_rmut);
      g_results.swap(res);
      g_total = total;
      if (g_cursor >= static_cast<int>(g_results.size()))
        g_cursor = g_results.empty() ? 0 : static_cast<int>(g_results.size()) - 1;
      if (g_scroll > g_cursor) g_scroll = g_cursor;
    } else {
      setStatus("搜索出错: " + err);
    }
  }
}

// 在 name 中高亮命中的子串（大小写不敏感）
void printHighlight(const std::string& name, const std::string& qlow) {
  if (qlow.empty()) {
    printw("%s", name.c_str());
    return;
  }
  std::string nlow = toLowerAscii(name);
  size_t pos = nlow.find(qlow);
  if (pos == std::string::npos) {
    printw("%s", name.c_str());
    return;
  }
  printw("%s", name.substr(0, pos).c_str());
  attron(A_BOLD);
  printw("%s", name.substr(pos, qlow.size()).c_str());
  attroff(A_BOLD);
  printw("%s", name.substr(pos + qlow.size()).c_str());
}

}  // namespace

int main() {
  setlocale(LC_ALL, "");
  initscr();
  cbreak();
  noecho();
  keypad(stdscr, TRUE);
  nodelay(stdscr, TRUE);
  curs_set(0);
  start_color();
  use_default_colors();
  init_pair(1, COLOR_CYAN, -1);     // 输入/状态
  init_pair(2, COLOR_WHITE, COLOR_BLUE);  // 选中行
  init_pair(3, COLOR_GREEN, -1);    // 目录
  mousemask(0, nullptr);

  int h = 0, w = 0;
  getmaxyx(stdscr, h, w);
  if (h < 3) h = 3;

  std::thread tw(worker);

  bool running = true;
  while (running) {
    getmaxyx(stdscr, h, w);
    const int rows = (h > 2) ? h - 2 : 1;

    // 输入行
    move(0, 0);
    clrtoeol();
    attron(COLOR_PAIR(1));
    printw("Search: %s", g_query.c_str());
    attroff(COLOR_PAIR(1));

    // 结果列表
    {
      std::lock_guard<std::mutex> lk(g_rmut);
      int n = static_cast<int>(g_results.size());
      if (g_scroll + rows > n && n > rows) g_scroll = n - rows;
      if (g_scroll < 0) g_scroll = 0;
      std::string qlow;
      { std::lock_guard<std::mutex> ql(g_qmut); qlow = toLowerAscii(g_query); }

      for (int r = 0; r < rows; ++r) {
        move(1 + r, 0);
        clrtoeol();
        int idx = g_scroll + r;
        if (idx >= n) {
          if (r == 0) printw("（输入关键词开始搜索 / 无结果）");
          continue;
        }
        const SearchResult& res = g_results[static_cast<size_t>(idx)];
        if (idx == g_cursor) attron(A_REVERSE);
        if (res.entry.is_dir) attron(COLOR_PAIR(3));
        printw("%s ", res.entry.is_dir ? "D " : "  ");
        printHighlight(res.entry.name, qlow);
        attroff(COLOR_PAIR(3));
        int used = getcurx(stdscr);
        if (used + 3 < w) {
          move(1 + r, used + 1);
          attron(A_DIM);
          printw("(%s)", res.entry.path.c_str());
          attroff(A_DIM);
        }
        if (idx == g_cursor) attroff(A_REVERSE);
      }
    }

    // 状态行
    move(h - 1, 0);
    clrtoeol();
    attron(COLOR_PAIR(1));
    {
      std::lock_guard<std::mutex> lk(g_statusM);
      printw("%s | 匹配 %zu%s | Enter 打开  F5 重建  Esc 清空  Ctrl+Q 退出",
             g_status.c_str(), g_total.load(), (g_total.load() > 2000 ? "+" : ""));
    }
    attroff(COLOR_PAIR(1));
    refresh();

    wint_t wc;
    int rv = get_wch(&wc);
    if (rv == ERR) {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
      continue;
    }
    int k = static_cast<int>(wc);

    if (k == KEY_RESIZE) {
      continue;
    } else if (k == KEY_UP) {
      std::lock_guard<std::mutex> lk(g_rmut);
      if (g_cursor > 0) {
        --g_cursor;
        if (g_cursor < g_scroll) g_scroll = g_cursor;
      }
    } else if (k == KEY_DOWN) {
      std::lock_guard<std::mutex> lk(g_rmut);
      if (g_cursor + 1 < static_cast<int>(g_results.size())) {
        ++g_cursor;
        if (g_cursor >= g_scroll + rows) ++g_scroll;
      }
    } else if (k == KEY_PPAGE) {
      std::lock_guard<std::mutex> lk(g_rmut);
      g_cursor -= rows;
      if (g_cursor < 0) g_cursor = 0;
      g_scroll = g_cursor;
    } else if (k == KEY_NPAGE) {
      std::lock_guard<std::mutex> lk(g_rmut);
      g_cursor += rows;
      if (g_cursor >= static_cast<int>(g_results.size()))
        g_cursor = g_results.empty() ? 0 : static_cast<int>(g_results.size()) - 1;
      g_scroll = g_cursor;
    } else if (k == KEY_HOME) {
      std::lock_guard<std::mutex> lk(g_rmut);
      g_cursor = 0;
      g_scroll = 0;
    } else if (k == KEY_END) {
      std::lock_guard<std::mutex> lk(g_rmut);
      if (!g_results.empty()) {
        g_cursor = static_cast<int>(g_results.size()) - 1;
        g_scroll = g_cursor;
      }
    } else if (k == KEY_F(5)) {
      triggerRebuild();
      setStatus("已触发重建…");
    } else if (k == '\n' || k == KEY_ENTER) {
      openSelected();
    } else if (k == 27) {  // Esc：清空
      std::lock_guard<std::mutex> lk(g_qmut);
      g_query.clear();
      g_needSearch = true;
    } else if (k == '\b' || k == 127 || k == KEY_BACKSPACE) {
      std::lock_guard<std::mutex> lk(g_qmut);
      if (!g_query.empty()) {
        // 删除最后一个 UTF-8 字符
        size_t i = g_query.size();
        while (i > 0 && (g_query[i - 1] & 0xC0) == 0x80) --i;
        g_query.erase(i - 1);
      }
      g_needSearch = true;
    } else if (k == 3 || k == 17) {  // Ctrl+C / Ctrl+Q
      running = false;
    } else if (rv != KEY_CODE_YES && k >= 32) {
      std::lock_guard<std::mutex> lk(g_qmut);
      g_query += wideToUtf8(wc);
      g_needSearch = true;
    }
  }

  g_run = false;
  if (tw.joinable()) tw.join();
  endwin();
  return 0;
}
