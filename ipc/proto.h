#pragma once
// IPC 明文协议（Unix domain socket，行分隔，可用 socat/nc 调试）
//
// 请求行（客户端 -> 守护进程）：
//   ping
//   version
//   stats
//   search <limit> <dirs_only> <files_only> <sort> <query...>
//   search2 <limit> <dirs_only> <files_only> <sort> <under64> <query...>
//   search3 <limit> <dirs_only> <files_only> <sort> <under64> <query...>
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
// 路径参数（Spec 013）：add-path 的参数**不做 CSV 分割**（整串即一个路径）；
//   set-paths / set-excludes 才按逗号取 CSV，且每个元素须合法。合法路径 =
//   非空、不含 ','、不含控制字符（<0x20 与 0x7f）、无首尾空格/TAB、长度 ≤ 4096。
//   remove-path 仅校验非空（以便精确移除历史含逗号条目），成功移除后**落盘**到
//   配置文件（重启不复活）。
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
//   OK <returned> <total> <total_capped> esc=1\n<结果行...>\nEND\n —— search3（path 转义）
//   OK <total> <total_capped>\n                      —— count2
//   OK\ncommands=<csv>\nEND\n                        —— capabilities
//   OK\n<key=value...>\nEND\n                        —— stats / version / get-config
// 结果行格式：path<TAB>is_dir<TAB>size<TAB>mtime<TAB>path_matched
//
// search3（帧完整性）：Linux 文件名可含 TAB/LF/CR，若原样写入结果行会撕裂帧
//   （LF 伪造 END、TAB 使字段错位）。search3 仅对 path 字段转义，转义表（顺序：
//   先 '\' 再其余，保证单射可逆）：
//     '\'  -> "\\"
//     TAB  -> "\t"
//     LF   -> "\n"
//     CR   -> "\r"
//   其余字段（is_dir/size/mtime/path_matched）为数字，不转义。仅支持 search3 的
//   客户端（capabilities 含 search3）才走此路径；旧客户端仍用 search2，字节不变。
//
// 错误目录（ERR <msg>，单行）：
//   ERR bad args              —— search 缺字段（splitHead 不足）
//   ERR bad sort              —— sort 不在 name/path/size/mtime
//   ERR bad regex: <msg>      —— re: 模式非法（<msg> 已折叠换行）
//   ERR bad under             —— search2/search3/count2 缺字段或 under64 非法
//   ERR unknown command       —— 未知动词
//   ERR paths must not be empty —— set-paths 传空
//   ERR bad path              —— 路径参数违反上述路径规则
//   ERR bad limit             —— search/search2/search3 的 limit 非纯十进制或 > 1048576
//   ERR bad opts              —— set-opts 参数不足
//   ERR line too long         —— 单行请求超过 1 MiB（随后关闭该连接）
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

// search3 结果行 path 字段的转义/还原（唯一实现处，daemon 编码、client 解码共用，
// 避免两处转义表漂移）。未识别的 "\x" 原样保留；unescape(escape(s)) == s。
std::string escapeField(const std::string& s);
std::string unescapeField(const std::string& s);

}  // namespace proto
}  // namespace lsearch
