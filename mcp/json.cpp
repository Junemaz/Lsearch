#include "mcp/json.h"

#include <cerrno>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace mcp {

namespace {

constexpr int kMaxDepth = 128;
constexpr const char* kReplacement = "\xEF\xBF\xBD";  // U+FFFD

int utf8SeqLen(unsigned char b) {
  if (b < 0x80) return 1;
  if ((b & 0xE0) == 0xC0) return 2;
  if ((b & 0xF0) == 0xE0) return 3;
  if ((b & 0xF8) == 0xF0) return 4;
  return 0;
}

// 校验 s[i] 起的 len 字节序列是否为合法 UTF-8（拒绝过长编码、代理区、越界码点）。
bool utf8Valid(const std::string& s, std::size_t i, int len) {
  if (len < 1 || i + static_cast<std::size_t>(len) > s.size()) return false;
  unsigned char b0 = static_cast<unsigned char>(s[i]);
  if (len == 1) return true;
  unsigned int cp = b0 & (0xFFu >> (len + 1));
  for (int k = 1; k < len; ++k) {
    unsigned char bk = static_cast<unsigned char>(s[i + static_cast<std::size_t>(k)]);
    if ((bk & 0xC0) != 0x80) return false;
    cp = (cp << 6) | (bk & 0x3Fu);
  }
  if (len == 2) return cp >= 0x80;
  if (len == 3) return cp >= 0x800 && !(cp >= 0xD800 && cp <= 0xDFFF);
  return cp >= 0x10000 && cp <= 0x10FFFF;
}

void appendUtf8(unsigned int cp, std::string& out) {
  if (cp <= 0x7F) {
    out.push_back(static_cast<char>(cp));
  } else if (cp <= 0x7FF) {
    out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else if (cp <= 0xFFFF) {
    out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else {
    out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  }
}

int hexVal(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

struct Parser {
  const std::string& s;
  std::size_t i = 0;
  std::string err;

  bool fail(const std::string& msg) {
    if (err.empty()) err = msg + " (offset " + std::to_string(i) + ")";
    return false;
  }
  void skipWs() {
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) ++i;
  }
  bool literal(const char* lit) {
    std::size_t n = std::strlen(lit);
    if (s.compare(i, n, lit) != 0) return fail("invalid literal");
    i += n;
    return true;
  }

  bool parseValue(Json& out, int depth) {
    if (depth > kMaxDepth) return fail("nesting too deep");
    skipWs();
    if (i >= s.size()) return fail("unexpected end of input");
    char c = s[i];
    if (c == '{') return parseObject(out, depth);
    if (c == '[') return parseArray(out, depth);
    if (c == '"') {
      std::string v;
      if (!parseString(v)) return false;
      out = Json::str(std::move(v));
      return true;
    }
    if (c == 't') {
      if (!literal("true")) return false;
      out = Json::boolean(true);
      return true;
    }
    if (c == 'f') {
      if (!literal("false")) return false;
      out = Json::boolean(false);
      return true;
    }
    if (c == 'n') {
      if (!literal("null")) return false;
      out = Json::makeNull();
      return true;
    }
    if (c == '-' || (c >= '0' && c <= '9')) return parseNumber(out);
    return fail("unexpected character");
  }

  bool parseString(std::string& out) {
    if (i >= s.size() || s[i] != '"') return fail("expected string");
    ++i;
    out.clear();
    while (true) {
      if (i >= s.size()) return fail("unterminated string");
      unsigned char c = static_cast<unsigned char>(s[i]);
      if (c == '"') {
        ++i;
        return true;
      }
      if (c == '\\') {
        ++i;
        if (i >= s.size()) return fail("unterminated escape");
        char e = s[i++];
        switch (e) {
          case '"': out.push_back('"'); break;
          case '\\': out.push_back('\\'); break;
          case '/': out.push_back('/'); break;
          case 'b': out.push_back('\b'); break;
          case 'f': out.push_back('\f'); break;
          case 'n': out.push_back('\n'); break;
          case 'r': out.push_back('\r'); break;
          case 't': out.push_back('\t'); break;
          case 'u': {
            unsigned int cp = 0;
            if (!readHex4(cp)) return false;
            if (cp >= 0xD800 && cp <= 0xDBFF) {
              // 高代理：尝试配对低代理 \uXXXX，否则按 U+FFFD 容错
              if (i + 1 < s.size() && s[i] == '\\' && s[i + 1] == 'u') {
                std::size_t save = i;
                i += 2;
                unsigned int lo = 0;
                if (readHex4(lo) && lo >= 0xDC00 && lo <= 0xDFFF) {
                  cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                  appendUtf8(cp, out);
                  break;
                }
                i = save;
              }
              appendUtf8(0xFFFD, out);
            } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
              appendUtf8(0xFFFD, out);
            } else {
              appendUtf8(cp, out);
            }
            break;
          }
          default: return fail("invalid escape");
        }
        continue;
      }
      if (c < 0x20) return fail("unescaped control character in string");
      out.push_back(static_cast<char>(c));
      ++i;
    }
  }

  bool readHex4(unsigned int& cp) {
    if (i + 4 > s.size()) return fail("bad \\u escape");
    cp = 0;
    for (int k = 0; k < 4; ++k) {
      int h = hexVal(s[i + static_cast<std::size_t>(k)]);
      if (h < 0) return fail("bad hex digit");
      cp = (cp << 4) | static_cast<unsigned int>(h);
    }
    i += 4;
    return true;
  }

  bool parseNumber(Json& out) {
    std::size_t start = i;
    if (i < s.size() && s[i] == '-') ++i;
    if (i >= s.size() || s[i] < '0' || s[i] > '9') return fail("invalid number");
    while (i < s.size() && s[i] >= '0' && s[i] <= '9') ++i;
    bool intLike = true;
    if (i < s.size() && s[i] == '.') {
      intLike = false;
      ++i;
      if (i >= s.size() || s[i] < '0' || s[i] > '9') return fail("invalid fraction");
      while (i < s.size() && s[i] >= '0' && s[i] <= '9') ++i;
    }
    if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
      intLike = false;
      ++i;
      if (i < s.size() && (s[i] == '+' || s[i] == '-')) ++i;
      if (i >= s.size() || s[i] < '0' || s[i] > '9') return fail("invalid exponent");
      while (i < s.size() && s[i] >= '0' && s[i] <= '9') ++i;
    }
    std::string tok = s.substr(start, i - start);
    if (intLike) {
      errno = 0;
      char* end = nullptr;
      long long v = std::strtoll(tok.c_str(), &end, 10);
      if (errno == 0 && end == tok.c_str() + tok.size()) {
        out = Json::integer(v);
        return true;
      }
    }
    double d = std::strtod(tok.c_str(), nullptr);
    if (!std::isfinite(d)) return fail("number out of range");
    out = Json::number(d);
    return true;
  }

  bool parseArray(Json& out, int depth) {
    ++i;  // '['
    out = Json::array();
    skipWs();
    if (i < s.size() && s[i] == ']') {
      ++i;
      return true;
    }
    while (true) {
      Json v;
      if (!parseValue(v, depth + 1)) return false;
      out.push(std::move(v));
      skipWs();
      if (i >= s.size()) return fail("unterminated array");
      if (s[i] == ',') {
        ++i;
        continue;
      }
      if (s[i] == ']') {
        ++i;
        return true;
      }
      return fail("expected ',' or ']'");
    }
  }

  bool parseObject(Json& out, int depth) {
    ++i;  // '{'
    out = Json::object();
    skipWs();
    if (i < s.size() && s[i] == '}') {
      ++i;
      return true;
    }
    while (true) {
      skipWs();
      std::string key;
      if (!parseString(key)) return false;
      skipWs();
      if (i >= s.size() || s[i] != ':') return fail("expected ':'");
      ++i;
      Json v;
      if (!parseValue(v, depth + 1)) return false;
      out.set(std::move(key), std::move(v));
      skipWs();
      if (i >= s.size()) return fail("unterminated object");
      if (s[i] == ',') {
        ++i;
        continue;
      }
      if (s[i] == '}') {
        ++i;
        return true;
      }
      return fail("expected ',' or '}'");
    }
  }
};

}  // namespace

Json Json::makeNull() { return Json(); }

Json Json::boolean(bool v) {
  Json j;
  j.type_ = Type::Bool;
  j.bool_ = v;
  return j;
}

Json Json::number(double v) {
  Json j;
  j.type_ = Type::Number;
  j.num_ = v;
  return j;
}

Json Json::integer(long long v) {
  Json j;
  j.type_ = Type::Number;
  j.intLike_ = true;
  j.int_ = v;
  j.num_ = static_cast<double>(v);
  return j;
}

Json Json::str(std::string v) {
  Json j;
  j.type_ = Type::String;
  j.str_ = std::move(v);
  return j;
}

Json Json::array() {
  Json j;
  j.type_ = Type::Array;
  return j;
}

Json Json::object() {
  Json j;
  j.type_ = Type::Object;
  return j;
}

bool Json::asBool(bool def) const { return type_ == Type::Bool ? bool_ : def; }

double Json::asNumber(double def) const { return type_ == Type::Number ? num_ : def; }

long long Json::asInt(long long def) const {
  if (type_ != Type::Number) return def;
  if (intLike_) return int_;
  if (!std::isfinite(num_)) return def;
  constexpr double kLLMin = -9223372036854775808.0;  // -2^63
  constexpr double kLLMax = 9223372036854775808.0;   //  2^63
  if (num_ >= kLLMax) return LLONG_MAX;
  if (num_ < kLLMin) return LLONG_MIN;
  return static_cast<long long>(num_);
}

const std::string& Json::asString() const {
  static const std::string kEmpty;
  return type_ == Type::String ? str_ : kEmpty;
}

std::size_t Json::size() const {
  if (type_ == Type::Array) return arr_.size();
  if (type_ == Type::Object) return obj_.size();
  return 0;
}

void Json::set(std::string key, Json v) {
  for (auto& kv : obj_) {
    if (kv.first == key) {
      kv.second = std::move(v);
      return;
    }
  }
  obj_.emplace_back(std::move(key), std::move(v));
}

const Json* Json::find(const std::string& key) const {
  for (const auto& kv : obj_)
    if (kv.first == key) return &kv.second;
  return nullptr;
}

bool Json::parse(const std::string& text, Json& out, std::string& err) {
  Parser p{text, 0, {}};
  Json v;
  if (!p.parseValue(v, 0)) {
    err = p.err;
    return false;
  }
  p.skipWs();
  if (p.i != text.size()) {
    err = "trailing characters (offset " + std::to_string(p.i) + ")";
    return false;
  }
  out = std::move(v);
  err.clear();
  return true;
}

std::string jsonEscape(const std::string& raw) {
  std::string out;
  out.reserve(raw.size() + 8);
  for (std::size_t i = 0; i < raw.size();) {
    unsigned char c = static_cast<unsigned char>(raw[i]);
    if (c < 0x80) {
      switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
          if (c < 0x20) {
            char buf[8];
            std::snprintf(buf, sizeof(buf), "\\u%04x", c);
            out += buf;
          } else {
            out.push_back(static_cast<char>(c));
          }
      }
      ++i;
      continue;
    }
    int len = utf8SeqLen(c);
    if (len > 1 && utf8Valid(raw, i, len)) {
      out.append(raw, i, static_cast<std::size_t>(len));
      i += static_cast<std::size_t>(len);
    } else {
      out += kReplacement;
      ++i;
    }
  }
  return out;
}

std::string Json::dump() const {
  switch (type_) {
    case Type::Null:
      return "null";
    case Type::Bool:
      return bool_ ? "true" : "false";
    case Type::Number: {
      if (intLike_) return std::to_string(int_);
      if (!std::isfinite(num_)) return "0";
      char buf[32];
      std::snprintf(buf, sizeof(buf), "%.17g", num_);
      return buf;
    }
    case Type::String:
      return "\"" + jsonEscape(str_) + "\"";
    case Type::Array: {
      std::string out = "[";
      for (std::size_t k = 0; k < arr_.size(); ++k) {
        if (k) out += ",";
        out += arr_[k].dump();
      }
      out += "]";
      return out;
    }
    case Type::Object: {
      std::string out = "{";
      for (std::size_t k = 0; k < obj_.size(); ++k) {
        if (k) out += ",";
        out += "\"" + jsonEscape(obj_[k].first) + "\":" + obj_[k].second.dump();
      }
      out += "}";
      return out;
    }
  }
  return "null";
}

}  // namespace mcp
