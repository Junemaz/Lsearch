#pragma once
// 最小 JSON 值/解析/序列化，仅覆盖 MCP 报文所需子集（object/array/string/number/
// bool/null）。不引入外部依赖；字符串序列化会转义控制字符并清洗非法 UTF-8。
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace mcp {

class Json {
 public:
  enum class Type { Null, Bool, Number, String, Array, Object };

  Json() = default;
  static Json makeNull();
  static Json boolean(bool v);
  static Json number(double v);
  static Json integer(long long v);
  static Json str(std::string v);
  static Json array();
  static Json object();

  Type type() const { return type_; }
  bool isNull() const { return type_ == Type::Null; }
  bool isBool() const { return type_ == Type::Bool; }
  bool isNumber() const { return type_ == Type::Number; }
  bool isString() const { return type_ == Type::String; }
  bool isArray() const { return type_ == Type::Array; }
  bool isObject() const { return type_ == Type::Object; }

  bool asBool(bool def = false) const;
  double asNumber(double def = 0) const;
  long long asInt(long long def = 0) const;
  const std::string& asString() const;

  const std::vector<Json>& items() const { return arr_; }
  void push(Json v) { arr_.push_back(std::move(v)); }
  std::size_t size() const;

  const std::vector<std::pair<std::string, Json>>& members() const { return obj_; }
  void set(std::string key, Json v);
  const Json* find(const std::string& key) const;
  bool has(const std::string& key) const { return find(key) != nullptr; }

  // 解析失败返回 false 并填 err；成功时 out 完整覆盖 text（含尾部空白检查）。
  static bool parse(const std::string& text, Json& out, std::string& err);
  // 生成合法 UTF-8 JSON 文本（非法 UTF-8 字节序列替换为 U+FFFD）。
  std::string dump() const;

 private:
  Type type_ = Type::Null;
  bool bool_ = false;
  bool intLike_ = false;
  long long int_ = 0;
  double num_ = 0;
  std::string str_;
  std::vector<Json> arr_;
  std::vector<std::pair<std::string, Json>> obj_;
};

// 将原始字节转义为 JSON 字符串内容（不含首尾引号），供 dump 与单测复用。
std::string jsonEscape(const std::string& raw);

}  // namespace mcp
