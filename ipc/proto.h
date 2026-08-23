#pragma once
// IPC 明文协议（Unix domain socket，行分隔，可用 socat/nc 调试）
//
// 请求行（客户端 -> 守护进程）：
//   ping
//   version
//   stats
//   search <limit> <dirs_only> <files_only> <sort> <query...>
//   rebuild
//   add-path <path>
//   remove-path <path>
//   shutdown
//   get-config                 ← 返回 paths/excludes/hidden/follow/config_file
//   set-paths <csv>            ← 整体替换索引根路径（逗号分隔，非空）
//   set-excludes <csv>         ← 整体替换排除前缀（"_" 表示清空）
//   set-opts <hidden> <follow> ← 隐藏文件/跟随符号链接（0|1）
//
// 响应（守护进程 -> 客户端）：
//   OK / ERR <msg>                                  —— 单行命令
//   OK <count>\n<结果行...>\nEND\n                   —— search
//   OK\n<key=value...>\nEND\n                        —— stats / version / get-config
// 结果行格式：path<TAB>is_dir<TAB>size<TAB>mtime<TAB>path_matched
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
