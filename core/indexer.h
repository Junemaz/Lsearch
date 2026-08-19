#pragma once
// 全量索引构建：并行目录遍历，产出 FileEntry 列表（供 SQLite + 内存索引使用）
#include "core/config.h"
#include "core/entry.h"

#include <atomic>
#include <functional>
#include <string>
#include <vector>

namespace lsearch {

struct ScanStats {
  std::atomic<uint64_t> files{0};
  std::atomic<uint64_t> dirs{0};
  std::atomic<uint64_t> errors{0};
};

using ScanProgress = std::function<void(const ScanStats&)>;

// 遍历 cfg.paths，把所有命中条目追加到 out（不会清空 out）。
// prog 可选，约每秒回调一次（用于守护进程打印进度）。
void fullScan(const Config& cfg, std::vector<FileEntry>& out, ScanProgress prog);

}
