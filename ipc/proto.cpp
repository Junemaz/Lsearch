#include "ipc/proto.h"

#include "core/util.h"

namespace lsearch {
namespace proto {

bool splitHead(const std::string& line, size_t headCount,
               std::vector<std::string>& head, std::string& rest) {
  head.clear();
  rest.clear();
  size_t pos = 0;
  while (head.size() < headCount && pos < line.size()) {
    while (pos < line.size() && (line[pos] == ' ' || line[pos] == '\t')) ++pos;
    if (pos >= line.size()) break;
    size_t start = pos;
    while (pos < line.size() && line[pos] != ' ' && line[pos] != '\t') ++pos;
    head.push_back(line.substr(start, pos - start));
  }
  while (pos < line.size() && (line[pos] == ' ' || line[pos] == '\t')) ++pos;
  rest = line.substr(pos);
  return head.size() == headCount;
}

bool parseResultLine(const std::string& line, bool& is_dir, long long& size,
                     long long& mtime, bool& path_matched, std::string& path) {
  auto parts = split(line, '\t', false);
  if (parts.size() < 5) return false;
  is_dir = (parts[1] == "1");
  size = atoll(parts[2].c_str());
  mtime = atoll(parts[3].c_str());
  path_matched = (parts[4] == "1");
  path = parts[0];
  return true;
}

std::string escapeField(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (char c : s) {
    switch (c) {
      case '\\': out += "\\\\"; break;
      case '\t': out += "\\t"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      default: out += c; break;
    }
  }
  return out;
}

std::string unescapeField(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '\\' && i + 1 < s.size()) {
      switch (s[i + 1]) {
        case '\\': out += '\\'; ++i; continue;
        case 't': out += '\t'; ++i; continue;
        case 'n': out += '\n'; ++i; continue;
        case 'r': out += '\r'; ++i; continue;
        default: break;
      }
    }
    out += s[i];
  }
  return out;
}

}  // namespace proto
}  // namespace lsearch
