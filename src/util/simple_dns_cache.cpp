
#include "util/simple_dns_cache.h"

#include <stdlib.h>

#include <chrono>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <utility>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#endif

#include "cos_defines.h"
#include "cos_sys_config.h"

namespace qcloud_cos {
namespace {

bool ResolveHost(const std::string& host, std::vector<std::string>* addresses) {
#if defined(_WIN32)
  static std::once_flag winsock_once;
  static int winsock_result = 0;
  std::call_once(winsock_once, []() {
    WSADATA data;
    winsock_result = WSAStartup(MAKEWORD(2, 2), &data);
  });
  if (winsock_result != 0) {
    return false;
  }
#endif

  addrinfo hints = {};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo* result = nullptr;
  if (getaddrinfo(host.c_str(), nullptr, &hints, &result) != 0) {
    return false;
  }

  char address_buffer[INET6_ADDRSTRLEN] = {};
  for (addrinfo* current = result; current != nullptr; current = current->ai_next) {
    const void* address = nullptr;
    if (current->ai_family == AF_INET) {
      address = &reinterpret_cast<sockaddr_in*>(current->ai_addr)->sin_addr;
    } else if (current->ai_family == AF_INET6) {
      address = &reinterpret_cast<sockaddr_in6*>(current->ai_addr)->sin6_addr;
    }
    if (address == nullptr) {
      continue;
    }

    if (inet_ntop(current->ai_family, address, address_buffer,
                  sizeof(address_buffer)) != nullptr) {
      addresses->push_back(address_buffer);
    }
  }
  freeaddrinfo(result);
  return !addresses->empty();
}

}  // namespace

SimpleDnsCache::SimpleDnsCache(unsigned max_size, unsigned expire_seconds)
    : m_max_size(max_size), m_expire_seconds(expire_seconds) {
  m_cache = std::make_shared<LruCache<std::string, HostEntryCache>>(m_max_size);
}

SimpleDnsCache::~SimpleDnsCache() {}

std::string SimpleDnsCache::Resolve(const std::string& host) {
  std::string ip_addr_str = "";
  if (host.empty()) {
    return ip_addr_str;
  }

  bool need_query_dns_server = false;
  HostEntryCache entry_cache;
  time_t current_ts = time(NULL);
  try {
    entry_cache = m_cache->Get(host);
    SDK_LOG_DBG("%s hit dns cache", host.c_str());
    if ((current_ts - entry_cache.cache_ts) > m_expire_seconds) {
      SDK_LOG_DBG("%s cache expired", host.c_str());
      need_query_dns_server = true;
    }
    // SDK_LOG_DBG("current_ts: %u, cache_ts: %u", current_ts,
    // entry_cache.cache_ts);
  } catch (const std::exception&) {
    SDK_LOG_DBG("%s not exists in cache", host.c_str());
    need_query_dns_server = true;
  }

  if (need_query_dns_server) {
    std::chrono::time_point<std::chrono::steady_clock> start_ts, end_ts;
    start_ts = std::chrono::steady_clock::now();
    std::vector<std::string> resolved_addresses;
    bool resolved = ResolveHost(host, &resolved_addresses);
    end_ts = std::chrono::steady_clock::now();
    unsigned time_consumed_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(end_ts - start_ts)
            .count();
    SDK_LOG_DBG("query dns server for host: %s, consume: %dms", host.c_str(),
                time_consumed_ms);
    if (resolved) {
      entry_cache.addresses = std::move(resolved_addresses);
      entry_cache.cache_ts = current_ts;
      m_cache->Put(host, entry_cache);
    } else {
      SDK_LOG_WARN("failed to resolve host: %s", host.c_str());
    }
  }

  size_t address_size = entry_cache.addresses.size();
  if (address_size > 0) {
    size_t slot = (std::hash<std::string>{}(host) +
                   static_cast<size_t>(current_ts)) % address_size;
    ip_addr_str = entry_cache.addresses[slot];
  }
  SDK_LOG_DBG("ip_addr_str: %s", ip_addr_str.c_str());
  return ip_addr_str;
}

bool SimpleDnsCache::Exist(const std::string& host) {
  return m_cache->Exist(host);
}

}  // namespace qcloud_cos
