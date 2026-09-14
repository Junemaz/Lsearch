#include "core/util.h"

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <ctime>
#include <pwd.h>
#include <cstdio>
#include <cstdlib>

namespace lsearch {

std::string toLowerAscii(const std::string& s) {
  std::string out = s;
  for (char& c : out)
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  return out;
}

std::string trim(const std::string& s) {
  size_t b = 0, e = s.size();
  while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n')) ++b;
  while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n')) --e;
  return s.substr(b, e - b);
}

std::vector<std::string> split(const std::string& s, char sep, bool skip_empty) {
  std::vector<std::string> out;
  size_t start = 0;
  while (start <= s.size()) {
    size_t pos = s.find(sep, start);
    if (pos == std::string::npos) pos = s.size();
    std::string tok = trim(s.substr(start, pos - start));
    if (!skip_empty || !tok.empty()) out.push_back(std::move(tok));
    start = pos + 1;
  }
  return out;
}

std::string joinPath(const std::string& a, const std::string& b) {
  if (a.empty()) return b;
  if (b.empty()) return a;
  if (a.back() == '/') {
    return (b.front() == '/') ? a + b.substr(1) : a + b;
  }
  return (b.front() == '/') ? a + b : a + "/" + b;
}

std::string joinList(const std::vector<std::string>& items, const std::string& sep) {
  std::string out;
  for (const auto& s : items) {
    if (s.empty()) continue;
    if (!out.empty()) out += sep;
    out += s;
  }
  return out;
}

std::string dirName(const std::string& path) {
  size_t pos = path.find_last_of('/');
  if (pos == std::string::npos) return ".";
  if (pos == 0) return "/";
  return path.substr(0, pos);
}

std::string baseName(const std::string& path) {
  size_t pos = path.find_last_of('/');
  if (pos == std::string::npos) return path;
  if (pos + 1 >= path.size()) return path;
  return path.substr(pos + 1);
}

bool startsWith(const std::string& s, const std::string& prefix) {
  return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

bool endsWith(const std::string& s, const std::string& suffix) {
  return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

static std::string envOr(const char* name, const std::string& fallback) {
  const char* v = getenv(name);
  return (v && *v) ? std::string(v) : fallback;
}

std::string homeDir() {
  const char* h = getenv("HOME");
  if (h && *h) return h;
  struct passwd* pw = getpwuid(getuid());
  if (pw && pw->pw_dir) return pw->pw_dir;
  return "/tmp";
}

std::string runtimeDir() {
  std::string rt = envOr("XDG_RUNTIME_DIR", "");
  if (!rt.empty()) return rt;
  char buf[64];
  snprintf(buf, sizeof(buf), "/tmp/lsearch-%d", static_cast<int>(getuid()));
  return buf;
}

static void ensureDir(const std::string& p) {
  // 自顶向下逐级创建（仅用于数据/配置目录），忽略已存在等无害错误
  std::string cur;
  size_t start = (p.front() == '/') ? 1 : 0;
  while (true) {
    size_t pos = p.find('/', start);
    cur = (pos == std::string::npos) ? p : p.substr(0, pos);
    if (!cur.empty()) mkdir(cur.c_str(), 0755);
    if (pos == std::string::npos) break;
    start = pos + 1;
  }
}

std::string dataDir() {
  std::string d = envOr("XDG_DATA_HOME", "");
  if (d.empty()) d = joinPath(homeDir(), ".local/share");
  d = joinPath(d, "lsearch");
  ensureDir(d);
  return d;
}

std::string configDir() {
  std::string d = envOr("XDG_CONFIG_HOME", "");
  if (d.empty()) d = joinPath(homeDir(), ".config");
  d = joinPath(d, "lsearch");
  ensureDir(d);
  return d;
}

int64_t nowSeconds() {
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  return static_cast<int64_t>(ts.tv_sec);
}

std::string humanSize(int64_t n) {
  const char* units[] = {"B", "KB", "MB", "GB", "TB"};
  double v = static_cast<double>(n);
  int u = 0;
  while (v >= 1024.0 && u < 4) {
    v /= 1024.0;
    ++u;
  }
  char buf[64];
  if (u == 0)
    snprintf(buf, sizeof(buf), "%lld B", static_cast<long long>(n));
  else
    snprintf(buf, sizeof(buf), "%.1f %s", v, units[u]);
  return buf;
}

std::string isoTime(int64_t t) {
  time_t tt = static_cast<time_t>(t);
  struct tm tmv;
  localtime_r(&tt, &tmv);
  char buf[32];
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tmv);
  return buf;
}

namespace {
int b64Value(unsigned char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;
}
}  // namespace

std::string base64Encode(const std::string& in) {
  static const char kTable[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve(((in.size() + 2) / 3) * 4);
  size_t i = 0;
  for (; i + 3 <= in.size(); i += 3) {
    unsigned n = (static_cast<unsigned>(static_cast<unsigned char>(in[i])) << 16) |
                 (static_cast<unsigned>(static_cast<unsigned char>(in[i + 1])) << 8) |
                 static_cast<unsigned>(static_cast<unsigned char>(in[i + 2]));
    out += kTable[(n >> 18) & 63];
    out += kTable[(n >> 12) & 63];
    out += kTable[(n >> 6) & 63];
    out += kTable[n & 63];
  }
  const size_t rem = in.size() - i;
  if (rem == 1) {
    unsigned n = static_cast<unsigned>(static_cast<unsigned char>(in[i])) << 16;
    out += kTable[(n >> 18) & 63];
    out += kTable[(n >> 12) & 63];
    out += "==";
  } else if (rem == 2) {
    unsigned n = (static_cast<unsigned>(static_cast<unsigned char>(in[i])) << 16) |
                 (static_cast<unsigned>(static_cast<unsigned char>(in[i + 1])) << 8);
    out += kTable[(n >> 18) & 63];
    out += kTable[(n >> 12) & 63];
    out += kTable[(n >> 6) & 63];
    out += '=';
  }
  return out;
}

bool base64Decode(const std::string& in, std::string& out) {
  out.clear();
  if (in.size() > kBase64MaxEncoded) return false;
  if (in.size() % 4 != 0) return false;

  std::string decoded;
  decoded.reserve((in.size() / 4) * 3);
  for (size_t i = 0; i < in.size(); i += 4) {
    const bool last = (i + 4 == in.size());
    int v[4] = {0, 0, 0, 0};
    int pad = 0;
    for (int k = 0; k < 4; ++k) {
      const unsigned char c = static_cast<unsigned char>(in[i + static_cast<size_t>(k)]);
      if (c == '=') {
        if (!last || k < 2) return false;
        ++pad;
        continue;
      }
      if (pad > 0) return false;
      v[k] = b64Value(c);
      if (v[k] < 0) return false;
    }
    if (pad > 2) return false;
    if (pad == 1 && (v[2] & 0x03) != 0) return false;
    if (pad == 2 && (v[1] & 0x0F) != 0) return false;
    const unsigned n = (static_cast<unsigned>(v[0]) << 18) |
                       (static_cast<unsigned>(v[1]) << 12) |
                       (static_cast<unsigned>(v[2]) << 6) | static_cast<unsigned>(v[3]);
    decoded += static_cast<char>((n >> 16) & 0xFF);
    if (pad < 2) decoded += static_cast<char>((n >> 8) & 0xFF);
    if (pad < 1) decoded += static_cast<char>(n & 0xFF);
  }
  if (decoded.find('\0') != std::string::npos) return false;
  out = std::move(decoded);
  return true;
}

bool validConfigPath(const std::string& p, std::string& why) {
  if (p.empty()) {
    why = "empty";
    return false;
  }
  if (p.size() > kConfigPathMax) {
    why = "too long";
    return false;
  }
  if (p.front() == ' ' || p.back() == ' ') {
    why = "leading/trailing space";
    return false;
  }
  for (unsigned char c : p) {
    if (c == ',') {
      why = "comma";
      return false;
    }
    if (c < 0x20 || c == 0x7f) {
      why = "control char";
      return false;
    }
  }
  why.clear();
  return true;
}

bool parseLimit(const std::string& s, std::size_t& out) {
  if (s.empty()) return false;
  std::size_t v = 0;
  for (unsigned char c : s) {
    if (c < '0' || c > '9') return false;
    v = v * 10 + static_cast<std::size_t>(c - '0');
    if (v > kLimitMax) return false;
  }
  out = v;
  return true;
}

bool globMatchView(const std::string& pattern, const char* text, std::size_t len) {
  // 经典递归 glob：* 匹配任意序列，? 匹配单个字符，大小写不敏感
  const std::string& p = pattern;
  const char* t = text;
  const size_t tlen = len;
  auto eq = [](char a, char b) {
    if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
    if (b >= 'A' && b <= 'Z') b = static_cast<char>(b - 'A' + 'a');
    return a == b;
  };
  size_t pi = 0, ti = 0;
  size_t star = std::string::npos, mark = 0;
  while (ti < tlen) {
    if (pi < p.size() && (eq(p[pi], t[ti]) || p[pi] == '?')) {
      ++pi;
      ++ti;
    } else if (pi < p.size() && p[pi] == '*') {
      star = pi++;
      mark = ti;
    } else if (star != std::string::npos) {
      pi = star + 1;
      ti = ++mark;
    } else {
      return false;
    }
  }
  while (pi < p.size() && p[pi] == '*') ++pi;
  return pi == p.size();
}

bool globMatch(const std::string& pattern, const std::string& text) {
  return globMatchView(pattern, text.data(), text.size());
}

}  // namespace lsearch
