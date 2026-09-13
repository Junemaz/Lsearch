#pragma once
// 通用小工具：字符串 / 路径 / 目录定位
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace lsearch {

std::string toLowerAscii(const std::string& s);
std::string trim(const std::string& s);
std::vector<std::string> split(const std::string& s, char sep, bool skip_empty = true);
std::string joinPath(const std::string& a, const std::string& b);
// 用 sep 拼接字符串列表（跳过空元素）
std::string joinList(const std::vector<std::string>& items, const std::string& sep);
std::string dirName(const std::string& path);
std::string baseName(const std::string& path);
bool startsWith(const std::string& s, const std::string& prefix);
bool endsWith(const std::string& s, const std::string& suffix);

// XDG 目录定位（无 XDG 环境变量时回退到 ~/.xxx）
std::string homeDir();
std::string runtimeDir();  // $XDG_RUNTIME_DIR 或 /tmp/lsearch-$UID
std::string dataDir();     // $XDG_DATA_HOME 或 ~/.local/share，并追加 /lsearch
std::string configDir();   // $XDG_CONFIG_HOME 或 ~/.config，并追加 /lsearch

int64_t nowSeconds();
std::string humanSize(int64_t n);   // 1234 -> "1.2 KB"
std::string isoTime(int64_t t);     // Unix 秒 -> "YYYY-MM-DD HH:MM:SS"

// 简单 glob：支持 * 和 ?，大小写不敏感
bool globMatch(const std::string& pattern, const std::string& text);

// 严格 base64（标准字母表 A-Za-z0-9+/ + '=' 填充）。
// 编码：无长度限制；解码：拒绝空白与非法字符、长度非 4 的倍数、非法填充，
// 编码串长度上限 kBase64MaxEncoded（8192），解码结果含 NUL 时失败。
// 失败时不修改 out（清空）。
std::string base64Encode(const std::string& in);
bool base64Decode(const std::string& in, std::string& out);
inline constexpr std::size_t kBase64MaxEncoded = 8192;

}  // namespace lsearch
