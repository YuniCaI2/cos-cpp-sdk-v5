#pragma once
#include <ctime>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "lru_cache.h"

namespace qcloud_cos {

struct HostEntryCache {
  std::vector<std::string> addresses;
  time_t cache_ts;
};

class SimpleDnsCache {
 public:
  using SharedLruCache = std::shared_ptr<LruCache<std::string, HostEntryCache>>;

  SimpleDnsCache(unsigned max_size, unsigned expire_seconds);
  ~SimpleDnsCache();
  // resolve host
  std::string Resolve(const std::string& host);
  bool Exist(const std::string& host);

 private:
  unsigned m_max_size;
  unsigned m_expire_seconds;
  SharedLruCache m_cache;
};
}  // namespace qcloud_cos
