#pragma once
// IPC 明文协议（Unix domain socket，行分隔，可用 socat/nc 调试）
//
// 请求行（客户端 -> 守护进程）：
//   ping
//   version
//   stats
//   search <limit> <dirs_only> <files_only> <sort> <query...>
//   search2 <limit> <dirs_only> <files_only> <sort> <under64> <query...>
//   count2 <dirs_only> <files_only> <under64> <query...>
//   capabilities
//   rebuild
//   add-path <path>
//   remove-path <path>
//   shutdown
//   get-config                 ← 返回 paths/excludes/hidden/follow/config_file
//   set-paths <csv>            ← 整体替换索引根路径（逗号分隔，非空）
//   set-excludes <csv>         ← 整体替换排除前缀（"_" 表示清空）
//   set-opts <hidden> <follow> ← 隐藏文件/跟随符号链接（0|1）
//
// under64：绝对路径子树前缀的**严格 base64**（标准字母表 + '=' 填充）；
//   空/全量用单字符 "-"（"-" 不是合法 base64，无歧义）。解码拒绝空白/非法字符/
//   非 4 倍数长度/超长（>8192）/含 NUL；解码后含控制字符（<0x20）亦视为非法。
//   匹配语义：path == under || path 以 under + "/" 开头（原始字节、大小写敏感）。
//
// 响应（守护进程 -> 客户端）：
//   OK / ERR <msg>                                  —— 单行命令
//   OK <count>\n<结果行...>\nEND\n                   —— search（旧语义：count=截断后条数）
//   OK <returned> <total> <total_capped>\n<结果行...>\nEND\n —— search2（total 精确或下界）
//   OK <total> <total_capped>\n                      —— count2
//   OK\ncommands=<csv>\nEND\n                        —— capabilities
//   OK\n<key=value...>\nEND\n                        —— stats / version / get-config
// 结果行格式：path<TAB>is_dir<TAB>size<TAB>mtime<TAB>path_matched
// 非法 under（含缺字段）→ "ERR bad under"；非法正则沿用 "ERR bad regex: ..."。
#include <cstddef>
#include <string>
#include <vector>

namespace lsearch {
namespace proto {

constexpr int kConnectTimeoutMs = 5000;

// 把一行拆成前 headCount 个空白分隔单词 + 其余部分（query 允许含空格）
bool splitHead(const std::string& line, size_t headCount,
               std::vector<std::string>& head, std::string& rest);

// 解析一条结果行 -> FileEntry
bool parseResultLine(const std::string& line, bool& is_dir, long long& size,
                     long long& mtime, bool& path_matched, std::string& path);

}  // namespace proto
}  // namespace lsearch
