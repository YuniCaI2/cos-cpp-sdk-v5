// Copyright (c) 2017, Tencent Inc.
// All rights reserved.
//
// Verifies that HttpSender actually reuses TCP connections when KeepAlive is
// on, by counting how many connections a loopback HTTP server accepts.

#include <atomic>
#include <cstring>
#include <map>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <curl/curl.h>

#include "cos_sys_config.h"
#include "gtest/gtest.h"
#include "util/curl_handle_pool.h"
#include "util/http_sender.h"

namespace qcloud_cos {
namespace {

#ifdef _WIN32
using SocketType = SOCKET;
const SocketType kInvalidSocket = INVALID_SOCKET;
void CloseSocket(SocketType fd) { closesocket(fd); }
#else
using SocketType = int;
const SocketType kInvalidSocket = -1;
void CloseSocket(SocketType fd) { close(fd); }
#endif

// Minimal HTTP/1.1 server on 127.0.0.1 that keeps connections open and counts
// how many it accepted. The accept count is the ground truth for reuse.
class LoopbackHttpServer {
 public:
  ~LoopbackHttpServer() { Stop(); }

  bool Start() {
#ifdef _WIN32
    WSADATA wsa_data;
    if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
      return false;
    }
    m_winsock_ready = true;
#endif
    m_listen_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (m_listen_fd == kInvalidSocket) {
      return false;
    }

    sockaddr_in address = {};
    address.sin_family = AF_INET;
    address.sin_port = 0;  // let the OS pick a free port
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(m_listen_fd, reinterpret_cast<sockaddr*>(&address),
             sizeof(address)) != 0) {
      return false;
    }
    if (listen(m_listen_fd, 16) != 0) {
      return false;
    }

    sockaddr_in bound = {};
#ifdef _WIN32
    int bound_len = sizeof(bound);
#else
    socklen_t bound_len = sizeof(bound);
#endif
    if (getsockname(m_listen_fd, reinterpret_cast<sockaddr*>(&bound),
                    &bound_len) != 0) {
      return false;
    }
    m_port = ntohs(bound.sin_port);

    m_thread = std::thread([this]() { AcceptLoop(); });
    return true;
  }

  void Stop() {
    if (m_stop.exchange(true)) {
      return;
    }
    if (m_thread.joinable()) {
      m_thread.join();
    }
    if (m_listen_fd != kInvalidSocket) {
      CloseSocket(m_listen_fd);
      m_listen_fd = kInvalidSocket;
    }
#ifdef _WIN32
    if (m_winsock_ready) {
      WSACleanup();
      m_winsock_ready = false;
    }
#endif
  }

  std::string Url() const {
    return "http://127.0.0.1:" + std::to_string(m_port) + "/";
  }

  int accepted() const { return m_accepted.load(); }
  int served() const { return m_served.load(); }

 private:
  static bool WaitReadable(SocketType fd, int timeout_ms) {
    fd_set read_set;
    FD_ZERO(&read_set);
    FD_SET(fd, &read_set);
    timeval timeout = {};
    timeout.tv_sec = timeout_ms / 1000;
    timeout.tv_usec = (timeout_ms % 1000) * 1000;
    return select(static_cast<int>(fd) + 1, &read_set, nullptr, nullptr,
                  &timeout) > 0;
  }

  void AcceptLoop() {
    while (!m_stop.load()) {
      if (!WaitReadable(m_listen_fd, 50)) {
        continue;
      }
      SocketType conn = accept(m_listen_fd, nullptr, nullptr);
      if (conn == kInvalidSocket) {
        continue;
      }
      ++m_accepted;
      ServeConnection(conn);
      CloseSocket(conn);
    }
  }

  void ServeConnection(SocketType conn) {
    static const char kResponse[] =
        "HTTP/1.1 200 OK\r\nContent-Length: 0\r\nConnection: keep-alive\r\n\r\n";
    std::string pending;
    while (!m_stop.load()) {
      if (!WaitReadable(conn, 50)) {
        continue;
      }
      char chunk[2048];
      const int received =
          static_cast<int>(recv(conn, chunk, sizeof(chunk), 0));
      if (received <= 0) {
        return;  // client closed the connection
      }
      pending.append(chunk, received);

      // GET/HEAD carry no body, so an empty line ends each request.
      size_t end = pending.find("\r\n\r\n");
      while (end != std::string::npos) {
        pending.erase(0, end + 4);
        send(conn, kResponse, static_cast<int>(sizeof(kResponse) - 1), 0);
        ++m_served;
        end = pending.find("\r\n\r\n");
      }
    }
  }

  SocketType m_listen_fd = kInvalidSocket;
  unsigned short m_port = 0;
  std::atomic<bool> m_stop{false};
  std::atomic<int> m_accepted{0};
  std::atomic<int> m_served{0};
  std::thread m_thread;
#ifdef _WIN32
  bool m_winsock_ready = false;
#endif
};

int SendOnce(const std::string& url, std::string* error) {
  const std::map<std::string, std::string> params;
  const std::map<std::string, std::string> headers;
  std::map<std::string, std::string> resp_headers;
  std::string resp_body;
  return HttpSender::SendRequest(nullptr, "GET", url, params, headers,
                                 std::string(), 3000, 3000, &resp_headers,
                                 &resp_body, error);
}

}  // namespace

class HttpSenderReuseTest : public ::testing::Test {
 protected:
  void SetUp() override {
    m_original_keep_alive = CosSysConfig::GetKeepAlive();
    m_original_pool_size = CosSysConfig::GetCurlHandlePoolSize();
    DrainPool();
    ASSERT_TRUE(m_server.Start());
  }

  void TearDown() override {
    m_server.Stop();
    // Pooled handles hold sockets to the server that is going away.
    DrainPool();
    CosSysConfig::SetKeepAlive(m_original_keep_alive);
    CosSysConfig::SetCurlHandlePoolSize(m_original_pool_size);
  }

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

  LoopbackHttpServer m_server;
  bool m_original_keep_alive = false;
  unsigned m_original_pool_size = 64;
};

TEST_F(HttpSenderReuseTest, KeepAliveOnReusesOneConnection) {
  CosSysConfig::SetKeepAlive(true);
  CosSysConfig::SetCurlHandlePoolSize(1);

  const int kRequests = 5;
  for (int i = 0; i < kRequests; ++i) {
    std::string error;
    EXPECT_EQ(200, SendOnce(m_server.Url(), &error))
        << "request " << i << " failed: " << error;
  }

  EXPECT_EQ(kRequests, m_server.served());
  EXPECT_EQ(1, m_server.accepted());
}

TEST_F(HttpSenderReuseTest, KeepAliveOffOpensOneConnectionPerRequest) {
  CosSysConfig::SetKeepAlive(false);

  const int kRequests = 5;
  for (int i = 0; i < kRequests; ++i) {
    std::string error;
    EXPECT_EQ(200, SendOnce(m_server.Url(), &error))
        << "request " << i << " failed: " << error;
  }

  EXPECT_EQ(kRequests, m_server.served());
  EXPECT_EQ(kRequests, m_server.accepted());
}

}  // namespace qcloud_cos
