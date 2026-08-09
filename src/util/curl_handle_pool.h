// Copyright (c) 2017, Tencent Inc.
// All rights reserved.
//
// Internal CURL easy-handle pool used by HttpSender for connection reuse.
// Not part of the public SDK API.

#ifndef COS_CPP_SDK_V5_SRC_UTIL_CURL_HANDLE_POOL_H_
#define COS_CPP_SDK_V5_SRC_UTIL_CURL_HANDLE_POOL_H_

#include <curl/curl.h>

#include <mutex>
#include <vector>

namespace qcloud_cos {

// Caches idle CURL easy handles so that libcurl can keep their established
// TCP/TLS connections alive between requests. A handle is owned by exactly one
// caller between Acquire() and Release(), which is what makes reuse safe while
// several SDK threads issue requests concurrently.
//
// Only the DNS and TLS session caches are shared across handles. The connection
// cache is deliberately not shared: libcurl does not support sharing
// connections between concurrent threads.
class CurlHandlePool {
 public:
  static CurlHandlePool& Instance();

  /// Acquire an easy handle. Pass allow_pooled=false for requests that need a
  /// private handle (no reuse, no shared caches).
  CURL* Acquire(bool allow_pooled);

  /// Return a handle to the pool, or destroy it when the transfer left the
  /// connection in an unknown state, the pool is full, or KeepAlive is off.
  void Release(CURL* handle, bool reusable);

  /// Whether the given libcurl result leaves the connection safe to reuse.
  static bool IsConnectionReusable(CURLcode code);

 private:
  CurlHandlePool();
  ~CurlHandlePool();

  CurlHandlePool(const CurlHandlePool&) = delete;
  CurlHandlePool& operator=(const CurlHandlePool&) = delete;

  void InitShare();
  void DestroyHandle(CURL* handle);

  static void ShareLock(CURL* handle, curl_lock_data data,
                        curl_lock_access access, void* userptr);
  static void ShareUnlock(CURL* handle, curl_lock_data data, void* userptr);

  CURLSH* m_share;
  std::mutex m_pool_mutex;
  std::vector<CURL*> m_handles;
  // One mutex per curl_lock_data value used by the share callbacks.
  std::mutex m_share_locks[CURL_LOCK_DATA_LAST];
};

}  // namespace qcloud_cos

#endif  // COS_CPP_SDK_V5_SRC_UTIL_CURL_HANDLE_POOL_H_
