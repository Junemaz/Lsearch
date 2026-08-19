#include "core/entry.h"

namespace lsearch {

const char* sortKeyName(SortKey k) {
  switch (k) {
    case SortKey::Name: return "name";
    case SortKey::Path: return "path";
    case SortKey::Size: return "size";
    case SortKey::Mtime: return "mtime";
  }
  return "name";
}

bool sortKeyFromName(const std::string& s, SortKey& out) {
  if (s == "name") { out = SortKey::Name; return true; }
  if (s == "path") { out = SortKey::Path; return true; }
  if (s == "size") { out = SortKey::Size; return true; }
  if (s == "mtime") { out = SortKey::Mtime; return true; }
  return false;
}

}  // namespace lsearch
