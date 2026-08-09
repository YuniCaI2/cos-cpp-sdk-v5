// getenv is the portable way to read the credentials below.
#define _CRT_SECURE_NO_WARNINGS

#include <stdlib.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "cos_api.h"
#include "cos_sys_config.h"

/**
 * 同 host 压测：对比 KeepAlive 关闭/开启两种情况下的请求吞吐与延迟，
 * 用来验证 CURL 句柄池带来的连接复用是否真的生效。
 *
 * 复用生效的表现：开启后平均延迟下降约一个 TCP+TLS 握手的耗时，QPS 相应上升。
 * 之所以用 HeadBucket，是因为它没有请求体和响应体，测出来的差异基本就是建连成本。
 *
 * 密钥不写在代码里，从环境变量读取：
 *   COS_SECRET_ID   必填
 *   COS_SECRET_KEY  必填
 *   COS_REGION      必填，例如 ap-guangzhou
 *   COS_BUCKET      必填，需带 appid 后缀，例如 examplebucket-1250000000
 *   COS_APPID       选填
 *   COS_ENDPOINT    选填，自定义完整域名（私有云/内网接入点），
 *                   例如 {bucket}.cos-internal.{region}.tencentcos.cn
 *   COS_USE_HTTPS   选填，设为 1 时走 https，默认 http
 *
 * 用法：keepalive_bench_demo [线程数] [每线程请求数]   默认 8 和 50
 */
using namespace qcloud_cos;

namespace {

struct PhaseStats {
  uint64_t ok = 0;
  uint64_t fail = 0;
  double wall_ms = 0.0;
  std::vector<double> latencies_ms;
  std::string first_error;
};

std::string ReadEnv(const char* name) {
  const char* value = getenv(name);
  return value == nullptr ? std::string() : std::string(value);
}

double Percentile(const std::vector<double>& sorted, double ratio) {
  if (sorted.empty()) {
    return 0.0;
  }
  size_t index = static_cast<size_t>(ratio * (sorted.size() - 1) + 0.5);
  return sorted[std::min(index, sorted.size() - 1)];
}

bool UseHttps() { return ReadEnv("COS_USE_HTTPS") == "1"; }

HeadBucketReq MakeRequest(const std::string& bucket) {
  HeadBucketReq req(bucket);
  if (UseHttps()) {
    req.SetHttps();
  }
  return req;
}

PhaseStats RunPhase(CosAPI& cos, const std::string& bucket, int threads,
                    int per_thread, bool keep_alive) {
  CosSysConfig::SetKeepAlive(keep_alive);

  std::vector<std::vector<double>> thread_latencies(threads);
  std::atomic<uint64_t> ok{0};
  std::atomic<uint64_t> fail{0};
  std::mutex error_mutex;
  std::string first_error;

  const auto phase_start = std::chrono::steady_clock::now();

  std::vector<std::thread> workers;
  workers.reserve(threads);
  for (int t = 0; t < threads; ++t) {
    workers.emplace_back([&, t]() {
      thread_latencies[t].reserve(per_thread);
      for (int n = 0; n < per_thread; ++n) {
        HeadBucketReq req = MakeRequest(bucket);
        HeadBucketResp resp;

        const auto started = std::chrono::steady_clock::now();
        const CosResult result = cos.HeadBucket(req, &resp);
        const auto finished = std::chrono::steady_clock::now();

        thread_latencies[t].push_back(
            std::chrono::duration<double, std::milli>(finished - started)
                .count());

        if (result.IsSucc()) {
          ++ok;
        } else {
          ++fail;
          std::lock_guard<std::mutex> lock(error_mutex);
          if (first_error.empty()) {
            first_error = "http_status=" +
                          std::to_string(result.GetHttpStatus()) + " code=" +
                          result.GetErrorCode() + " msg=" +
                          result.GetErrorMsg();
          }
        }
      }
    });
  }
  for (auto& worker : workers) {
    worker.join();
  }

  const auto phase_end = std::chrono::steady_clock::now();

  PhaseStats stats;
  stats.ok = ok.load();
  stats.fail = fail.load();
  stats.wall_ms =
      std::chrono::duration<double, std::milli>(phase_end - phase_start)
          .count();
  stats.first_error = first_error;
  for (const auto& latencies : thread_latencies) {
    stats.latencies_ms.insert(stats.latencies_ms.end(), latencies.begin(),
                              latencies.end());
  }
  std::sort(stats.latencies_ms.begin(), stats.latencies_ms.end());
  return stats;
}

void PrintPhase(const std::string& title, const PhaseStats& stats) {
  double sum = 0.0;
  for (double latency : stats.latencies_ms) {
    sum += latency;
  }
  const double avg =
      stats.latencies_ms.empty() ? 0.0 : sum / stats.latencies_ms.size();
  const double qps =
      stats.wall_ms > 0.0 ? (stats.ok + stats.fail) * 1000.0 / stats.wall_ms
                          : 0.0;

  std::cout << std::fixed << std::setprecision(2);
  std::cout << title << std::endl;
  std::cout << "  成功/失败: " << stats.ok << " / " << stats.fail << std::endl;
  std::cout << "  总耗时:    " << stats.wall_ms << " ms" << std::endl;
  std::cout << "  QPS:       " << qps << std::endl;
  std::cout << "  延迟 avg:  " << avg << " ms" << std::endl;
  std::cout << "  延迟 p50:  " << Percentile(stats.latencies_ms, 0.50) << " ms"
            << std::endl;
  std::cout << "  延迟 p95:  " << Percentile(stats.latencies_ms, 0.95) << " ms"
            << std::endl;
  std::cout << "  延迟 p99:  " << Percentile(stats.latencies_ms, 0.99) << " ms"
            << std::endl;
  if (!stats.first_error.empty()) {
    std::cout << "  首个错误:  " << stats.first_error << std::endl;
  }
}

double AverageOf(const PhaseStats& stats) {
  if (stats.latencies_ms.empty()) {
    return 0.0;
  }
  double sum = 0.0;
  for (double latency : stats.latencies_ms) {
    sum += latency;
  }
  return sum / stats.latencies_ms.size();
}

}  // namespace

int main(int argc, char** argv) {
  const std::string secret_id = ReadEnv("COS_SECRET_ID");
  const std::string secret_key = ReadEnv("COS_SECRET_KEY");
  const std::string region = ReadEnv("COS_REGION");
  const std::string bucket = ReadEnv("COS_BUCKET");
  if (secret_id.empty() || secret_key.empty() || region.empty() ||
      bucket.empty()) {
    std::cerr << "请先设置环境变量 COS_SECRET_ID / COS_SECRET_KEY / "
                 "COS_REGION / COS_BUCKET"
              << std::endl;
    return 1;
  }
  const uint64_t appid =
      static_cast<uint64_t>(atoll(ReadEnv("COS_APPID").c_str()));

  const int threads = argc > 1 ? std::max(1, atoi(argv[1])) : 8;
  const int per_thread = argc > 2 ? std::max(1, atoi(argv[2])) : 50;

  CosSysConfig::SetLogLevel(static_cast<LOG_LEVEL>(COS_LOG_ERR));
  // 句柄池至少要能容纳所有工作线程，否则归还时会因为超出上限被销毁，连接就复用不上。
  CosSysConfig::SetCurlHandlePoolSize(static_cast<unsigned>(threads));

  CosConfig config(appid, secret_id, secret_key, region);
  const std::string endpoint = ReadEnv("COS_ENDPOINT");
  if (!endpoint.empty()) {
    // 自定义域名同时作为请求 URL 的 host 和签名用的 Host 头。
    config.SetDestDomain(endpoint);
    config.SetDomainSameToHost(true);
  }
  CosAPI cos(config);

  std::cout << "bucket=" << bucket << " region=" << region << std::endl
            << "endpoint=" << (endpoint.empty() ? "(默认域名)" : endpoint)
            << " 协议=" << (UseHttps() ? "https" : "http") << std::endl
            << "线程数=" << threads << " 每线程请求数=" << per_thread
            << " 总请求数=" << threads * per_thread << std::endl
            << std::endl;

  // 预热：先把 DNS 解析和首次 TLS 协商的开销跑掉，避免算进第一个阶段。
  // 这里用 GetBucket 而不是 HeadBucket，因为 HEAD 没有响应体，失败时看不到
  // 服务端给的错误信息。
  {
    GetBucketReq req(bucket);
    req.SetMaxKeys(1);
    if (UseHttps()) {
      req.SetHttps();
    }
    GetBucketResp resp;
    const CosResult warmup = cos.GetBucket(req, &resp);
    if (!warmup.IsSucc()) {
      std::cerr << "预热请求失败，请检查密钥/地域/存储桶配置:" << std::endl
                << "  http_status = " << warmup.GetHttpStatus() << std::endl
                << "  error_code  = " << warmup.GetErrorCode() << std::endl
                << "  error_msg   = " << warmup.GetErrorMsg() << std::endl
                << "  request_id  = " << warmup.GetXCosRequestId() << std::endl;
      return 1;
    }
  }

  // 先跑关闭，避免开启阶段留在池里的连接影响到对照组。
  const PhaseStats without_reuse =
      RunPhase(cos, bucket, threads, per_thread, false);
  PrintPhase("[KeepAlive=false] 每次请求新建连接", without_reuse);
  std::cout << std::endl;

  const PhaseStats with_reuse = RunPhase(cos, bucket, threads, per_thread, true);
  PrintPhase("[KeepAlive=true] 复用句柄池中的连接", with_reuse);
  std::cout << std::endl;

  const double avg_off = AverageOf(without_reuse);
  const double avg_on = AverageOf(with_reuse);
  std::cout << std::fixed << std::setprecision(2);
  if (avg_off > 0.0 && avg_on > 0.0 && with_reuse.wall_ms > 0.0 &&
      without_reuse.wall_ms > 0.0) {
    std::cout << "平均延迟下降 " << (avg_off - avg_on) << " ms ("
              << (avg_off - avg_on) / avg_off * 100.0 << "%)" << std::endl;
    std::cout << "吞吐提升 " << without_reuse.wall_ms / with_reuse.wall_ms
              << " 倍" << std::endl;
  }
  if (with_reuse.fail > 0 || without_reuse.fail > 0) {
    std::cout << "存在失败请求，上面的对比结果仅供参考" << std::endl;
  }
  return 0;
}
