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
// cancel 可选：非空时遍历会定期检查，置真则尽快中止（out 内容不确定，调用方须丢弃）。
// 用于守护进程"管理命令打断在飞重建"（Spec 015）；默认 nullptr = 不可取消。
// 测试延迟钩子见 LSEARCH_SCAN_DELAY_US（core/indexer.cpp，Spec 015）。
void fullScan(const Config& cfg, std::vector<FileEntry>& out, ScanProgress prog,
              const std::atomic<bool>* cancel = nullptr);

}  // namespace lsearch
