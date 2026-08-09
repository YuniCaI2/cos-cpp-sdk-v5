// Copyright (c) 2017, Tencent Inc.
// All rights reserved.

#include <atomic>
#include <thread>
#include <vector>

#include <curl/curl.h>

#include "cos_sys_config.h"
#include "gtest/gtest.h"
#include "util/curl_handle_pool.h"

namespace qcloud_cos {

class CurlHandlePoolTest : public ::testing::Test {
 protected:
  static void SetUpTestSuite() {
    ASSERT_EQ(CURLE_OK, curl_global_init(CURL_GLOBAL_DEFAULT));
  }

  void SetUp() override {
    original_keep_alive_ = CosSysConfig::GetKeepAlive();
    original_pool_size_ = CosSysConfig::GetCurlHandlePoolSize();
    DrainPool();
  }

  void TearDown() override {
    DrainPool();
    CosSysConfig::SetKeepAlive(original_keep_alive_);
    CosSysConfig::SetCurlHandlePoolSize(original_pool_size_);
  }

  // The pool is a process-wide singleton, so each test starts from an empty
  // free list instead of inheriting handles from the previous one.
  static void DrainPool() {
    const size_t count = CosSysConfig::GetCurlHandlePoolSize() + 8;
    std::vector<CURL*> handles;
    handles.reserve(count);
    for (size_t i = 0; i < count; ++i) {
      handles.push_back(CurlHandlePool::Instance().Acquire(true));
    }
    for (CURL* handle : handles) {
      CurlHandlePool::Instance().Release(handle, false);
    }
  }

  bool original_keep_alive_ = false;
  unsigned original_pool_size_ = 64;
};

TEST_F(CurlHandlePoolTest, IsConnectionReusable) {
  EXPECT_TRUE(CurlHandlePool::IsConnectionReusable(CURLE_OK));
  EXPECT_FALSE(CurlHandlePool::IsConnectionReusable(CURLE_COULDNT_CONNECT));
  EXPECT_FALSE(CurlHandlePool::IsConnectionReusable(CURLE_RECV_ERROR));
  EXPECT_FALSE(CurlHandlePool::IsConnectionReusable(CURLE_SEND_ERROR));
  EXPECT_FALSE(CurlHandlePool::IsConnectionReusable(CURLE_SSL_CONNECT_ERROR));
  EXPECT_FALSE(CurlHandlePool::IsConnectionReusable(CURLE_OPERATION_TIMEDOUT));
  EXPECT_FALSE(CurlHandlePool::IsConnectionReusable(CURLE_ABORTED_BY_CALLBACK));
}

TEST_F(CurlHandlePoolTest, PoolSizeIsClampedToAtLeastOne) {
  CosSysConfig::SetCurlHandlePoolSize(0);
  EXPECT_EQ(1u, CosSysConfig::GetCurlHandlePoolSize());

  CosSysConfig::SetCurlHandlePoolSize(16);
  EXPECT_EQ(16u, CosSysConfig::GetCurlHandlePoolSize());
}

TEST_F(CurlHandlePoolTest, KeepAliveOnReusesHandles) {
  CosSysConfig::SetKeepAlive(true);
  CosSysConfig::SetCurlHandlePoolSize(8);

  CURL* first = CurlHandlePool::Instance().Acquire(true);
  ASSERT_NE(nullptr, first);
  CurlHandlePool::Instance().Release(first, true);

  CURL* second = CurlHandlePool::Instance().Acquire(true);
  ASSERT_NE(nullptr, second);
  EXPECT_EQ(first, second);
  CurlHandlePool::Instance().Release(second, true);
}

TEST_F(CurlHandlePoolTest, KeepAliveOffDoesNotUseThePool) {
  // Seed one handle into the pool while KeepAlive is on.
  CosSysConfig::SetKeepAlive(true);
  CURL* pooled = CurlHandlePool::Instance().Acquire(true);
  ASSERT_NE(nullptr, pooled);
  CurlHandlePool::Instance().Release(pooled, true);

  // With KeepAlive off, Acquire must not take the pooled handle and Release
  // must destroy instead of publishing back.
  CosSysConfig::SetKeepAlive(false);
  CURL* temporary = CurlHandlePool::Instance().Acquire(true);
  ASSERT_NE(nullptr, temporary);
  EXPECT_NE(pooled, temporary);
  CurlHandlePool::Instance().Release(temporary, true);

  // Re-enabling KeepAlive still yields the originally pooled handle, proving it
  // stayed in the free list and that `temporary` was never added.
  CosSysConfig::SetKeepAlive(true);
  CURL* again = CurlHandlePool::Instance().Acquire(true);
  ASSERT_NE(nullptr, again);
  EXPECT_EQ(pooled, again);
  CurlHandlePool::Instance().Release(again, true);
}

TEST_F(CurlHandlePoolTest, PrivateHandleBypassesThePool) {
  CosSysConfig::SetKeepAlive(true);

  CURL* pooled = CurlHandlePool::Instance().Acquire(true);
  ASSERT_NE(nullptr, pooled);
  CurlHandlePool::Instance().Release(pooled, true);

  CURL* private_handle = CurlHandlePool::Instance().Acquire(false);
  ASSERT_NE(nullptr, private_handle);
  EXPECT_NE(pooled, private_handle);
  // HttpSender never reports a private handle as reusable.
  CurlHandlePool::Instance().Release(private_handle, false);

  CURL* again = CurlHandlePool::Instance().Acquire(true);
  ASSERT_NE(nullptr, again);
  EXPECT_EQ(pooled, again);
  CurlHandlePool::Instance().Release(again, true);
}

TEST_F(CurlHandlePoolTest, NonReusableHandleIsNotPooled) {
  CosSysConfig::SetKeepAlive(true);

  CURL* doomed = CurlHandlePool::Instance().Acquire(true);
  ASSERT_NE(nullptr, doomed);
  CurlHandlePool::Instance().Release(doomed, false);

  CURL* kept = CurlHandlePool::Instance().Acquire(true);
  ASSERT_NE(nullptr, kept);
  CurlHandlePool::Instance().Release(kept, true);

  // Only `kept` may be in the pool; `doomed` must have been destroyed.
  CURL* again = CurlHandlePool::Instance().Acquire(true);
  ASSERT_NE(nullptr, again);
  EXPECT_EQ(kept, again);
  CurlHandlePool::Instance().Release(again, true);
}

TEST_F(CurlHandlePoolTest, ReusedHandleIsUsableAfterReset) {
  CosSysConfig::SetKeepAlive(true);

  CURL* first = CurlHandlePool::Instance().Acquire(true);
  ASSERT_NE(nullptr, first);
  ASSERT_EQ(CURLE_OK, curl_easy_setopt(first, CURLOPT_URL, "http://localhost/"));
  CurlHandlePool::Instance().Release(first, true);

  CURL* second = CurlHandlePool::Instance().Acquire(true);
  ASSERT_EQ(first, second);
  // Options were cleared by the reset, so the handle accepts a fresh setup.
  EXPECT_EQ(CURLE_OK, curl_easy_setopt(second, CURLOPT_NOSIGNAL, 1L));
  EXPECT_EQ(CURLE_OK, curl_easy_setopt(second, CURLOPT_URL, "http://127.0.0.1/"));
  CurlHandlePool::Instance().Release(second, true);
}

TEST_F(CurlHandlePoolTest, ConcurrentAcquireRelease) {
  CosSysConfig::SetKeepAlive(true);
  CosSysConfig::SetCurlHandlePoolSize(32);

  constexpr int kThreads = 8;
  constexpr int kLoops = 50;
  std::atomic<int> acquired{0};
  std::vector<std::thread> threads;
  threads.reserve(kThreads);

  for (int i = 0; i < kThreads; ++i) {
    threads.emplace_back([&, i]() {
      for (int n = 0; n < kLoops; ++n) {
        const bool allow_pooled = (i % 2) == 0;
        CURL* handle = CurlHandlePool::Instance().Acquire(allow_pooled);
        if (handle == nullptr) {
          return;
        }
        curl_easy_setopt(handle, CURLOPT_NOSIGNAL, 1L);
        CurlHandlePool::Instance().Release(handle, allow_pooled);
        ++acquired;
      }
    });
  }

  for (auto& t : threads) {
    t.join();
  }
  EXPECT_EQ(kThreads * kLoops, acquired.load());
  EXPECT_GE(CosSysConfig::GetCurlHandlePoolSize(), 1u);
}

}  // namespace qcloud_cos
