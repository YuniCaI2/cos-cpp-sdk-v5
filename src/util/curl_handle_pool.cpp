// Copyright (c) 2017, Tencent Inc.
// All rights reserved.

#include "util/curl_handle_pool.h"

#include "cos_sys_config.h"

namespace qcloud_cos {

CurlHandlePool& CurlHandlePool::Instance() {
  static CurlHandlePool pool;
  return pool;
}

CurlHandlePool::CurlHandlePool() : m_share(nullptr) { InitShare(); }

CurlHandlePool::~CurlHandlePool() {
  {
    std::lock_guard<std::mutex> lock(m_pool_mutex);
    for (CURL* handle : m_handles) {
      DestroyHandle(handle);
    }
    m_handles.clear();
  }

  // curl_share_cleanup refuses to run while handles are still attached, so it
  // has to happen after every pooled handle has been detached and destroyed.
  if (m_share != nullptr) {
    curl_share_cleanup(m_share);
    m_share = nullptr;
  }
}

void CurlHandlePool::InitShare() {
  m_share = curl_share_init();
  if (m_share == nullptr) {
    return;
  }

  curl_share_setopt(m_share, CURLSHOPT_LOCKFUNC, &CurlHandlePool::ShareLock);
  curl_share_setopt(m_share, CURLSHOPT_UNLOCKFUNC,
                    &CurlHandlePool::ShareUnlock);
  curl_share_setopt(m_share, CURLSHOPT_USERDATA, this);
  curl_share_setopt(m_share, CURLSHOPT_SHARE, CURL_LOCK_DATA_DNS);
  curl_share_setopt(m_share, CURLSHOPT_SHARE, CURL_LOCK_DATA_SSL_SESSION);
}

void CurlHandlePool::ShareLock(CURL* /*handle*/, curl_lock_data data,
                               curl_lock_access /*access*/, void* userptr) {
  auto* pool = static_cast<CurlHandlePool*>(userptr);
  if (pool == nullptr || data >= CURL_LOCK_DATA_LAST) {
    return;
  }
  pool->m_share_locks[data].lock();
}

void CurlHandlePool::ShareUnlock(CURL* /*handle*/, curl_lock_data data,
                                 void* userptr) {
  auto* pool = static_cast<CurlHandlePool*>(userptr);
  if (pool == nullptr || data >= CURL_LOCK_DATA_LAST) {
    return;
  }
  pool->m_share_locks[data].unlock();
}

void CurlHandlePool::DestroyHandle(CURL* handle) {
  if (handle == nullptr) {
    return;
  }
  curl_easy_setopt(handle, CURLOPT_SHARE, nullptr);
  curl_easy_cleanup(handle);
}

bool CurlHandlePool::IsConnectionReusable(CURLcode code) {
  // Only a cleanly finished transfer leaves the connection in a known state.
  // HTTP errors such as 4xx/5xx still return CURLE_OK, so they keep the
  // connection; timeouts, aborts and transport failures do not.
  return code == CURLE_OK;
}

CURL* CurlHandlePool::Acquire(bool allow_pooled) {
  const bool pooling = allow_pooled && CosSysConfig::GetKeepAlive();

  CURL* handle = nullptr;
  if (pooling) {
    std::lock_guard<std::mutex> lock(m_pool_mutex);
    if (!m_handles.empty()) {
      handle = m_handles.back();
      m_handles.pop_back();
    }
  }

  if (handle != nullptr) {
    // Clears options only; live connections, the DNS/TLS caches and the share
    // binding survive, which is what makes the connection reusable.
    curl_easy_reset(handle);
    return handle;
  }

  handle = curl_easy_init();
  if (handle == nullptr) {
    return nullptr;
  }
  if (pooling && m_share != nullptr) {
    curl_easy_setopt(handle, CURLOPT_SHARE, m_share);
  }
  return handle;
}

void CurlHandlePool::Release(CURL* handle, bool reusable) {
  if (handle == nullptr) {
    return;
  }

  if (!reusable || !CosSysConfig::GetKeepAlive()) {
    DestroyHandle(handle);
    return;
  }

  // Reset before publishing the handle: the finished request configured it with
  // pointers to its own stack (error buffer, callback contexts).
  curl_easy_reset(handle);

  std::lock_guard<std::mutex> lock(m_pool_mutex);
  if (m_handles.size() >= CosSysConfig::GetCurlHandlePoolSize()) {
    DestroyHandle(handle);
    return;
  }
  m_handles.push_back(handle);
}

}  // namespace qcloud_cos
