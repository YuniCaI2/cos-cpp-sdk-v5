// Copyright (c) 2017, Tencent Inc.
// All rights reserved.
//
// The transport implementation is intentionally private.  Public SDK headers
// expose streams and callbacks, while libcurl/OpenSSL stay implementation
// details of the SDK target.

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "util/http_sender.h"

#include <curl/curl.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <inttypes.h>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>

#include "cos_sys_config.h"
#include "internal/crypto_util.h"
#include "util/codec_util.h"
#include "util/curl_handle_pool.h"
#include "util/string_util.h"

namespace qcloud_cos {
namespace {

struct CurlRequestContext {
  SharedTransferHandler handler;
  std::istream* request_stream = nullptr;
  const char* request_buffer = nullptr;
  size_t request_length = 0;
  size_t request_offset = 0;
  std::ostream* response_stream = nullptr;
  std::string* captured_response = nullptr;
  bool capture_response = false;
  bool request_failed = false;
  bool response_failed = false;
  bool callback_cancelled = false;
  uint64_t sent_bytes = 0;
  uint64_t received_bytes = 0;
};

struct SslCallbackContext {
  const SSLCtxCallback* callback = nullptr;
  void* user_data = nullptr;
  std::string error;
};

struct CurlSlistDeleter {
  void operator()(curl_slist* headers) const {
    if (headers != nullptr) {
      curl_slist_free_all(headers);
    }
  }
};

using CurlSlist = std::unique_ptr<curl_slist, CurlSlistDeleter>;

// RAII wrapper that returns the easy handle to CurlHandlePool. A handle taken
// with allow_pooled=false is never published back to the pool.
class PooledCurlHandle {
 public:
  explicit PooledCurlHandle(bool allow_pooled)
      : m_handle(CurlHandlePool::Instance().Acquire(allow_pooled)),
        m_allow_pooled(allow_pooled),
        m_reusable(false) {}

  ~PooledCurlHandle() {
    if (m_handle != nullptr) {
      CurlHandlePool::Instance().Release(m_handle,
                                         m_allow_pooled && m_reusable);
      m_handle = nullptr;
    }
  }

  PooledCurlHandle(const PooledCurlHandle&) = delete;
  PooledCurlHandle& operator=(const PooledCurlHandle&) = delete;

  CURL* get() const { return m_handle; }
  explicit operator bool() const { return m_handle != nullptr; }

  void SetReusable(bool reusable) { m_reusable = reusable; }

 private:
  CURL* m_handle;
  bool m_allow_pooled;
  bool m_reusable;
};

CURLcode EnsureCurlInitialized() {
  static std::once_flag init_once;
  static CURLcode init_result = CURLE_FAILED_INIT;
  std::call_once(init_once, []() {
    init_result = curl_global_init(CURL_GLOBAL_DEFAULT);
  });
  return init_result;
}

int HexValue(char value) {
  if (value >= '0' && value <= '9') return value - '0';
  if (value >= 'a' && value <= 'f') return value - 'a' + 10;
  if (value >= 'A' && value <= 'F') return value - 'A' + 10;
  return -1;
}

std::string PercentDecode(const std::string& value) {
  std::string decoded;
  decoded.reserve(value.size());
  for (size_t i = 0; i < value.size(); ++i) {
    if (value[i] == '%' && i + 2 < value.size()) {
      int high = HexValue(value[i + 1]);
      int low = HexValue(value[i + 2]);
      if (high >= 0 && low >= 0) {
        decoded.push_back(static_cast<char>((high << 4) | low));
        i += 2;
        continue;
      }
    }
    decoded.push_back(value[i]);
  }
  return decoded;
}

// BaseOpUtil already percent-encodes the object path. Decode and encode it
// once here to preserve the previous URI behavior and avoid %25 double
// escaping when the path contains non-ASCII characters.
std::string BuildRequestUrl(
    const std::string& url,
    const std::map<std::string, std::string>& request_params) {
  const size_t scheme_end = url.find("://");
  const size_t authority_begin =
      scheme_end == std::string::npos ? 0 : scheme_end + 3;
  size_t path_begin = url.find_first_of("/?#", authority_begin);
  if (path_begin == std::string::npos) {
    path_begin = url.size();
  }

  size_t query_begin = url.find('?', path_begin);
  size_t fragment_begin = url.find('#', path_begin);
  if (query_begin != std::string::npos &&
      fragment_begin != std::string::npos && query_begin > fragment_begin) {
    query_begin = std::string::npos;
  }

  size_t path_end = url.size();
  if (query_begin != std::string::npos) {
    path_end = query_begin;
  } else if (fragment_begin != std::string::npos) {
    path_end = fragment_begin;
  }

  std::string raw_path = url.substr(path_begin, path_end - path_begin);
  if (raw_path.empty()) {
    raw_path = "/";
  }
  std::string request_url = url.substr(0, path_begin) +
                            CodecUtil::EncodeKey(PercentDecode(raw_path));

  std::string query;
  if (query_begin != std::string::npos) {
    const size_t query_end = fragment_begin == std::string::npos
                                 ? url.size()
                                 : fragment_begin;
    query = url.substr(query_begin + 1, query_end - query_begin - 1);
  }

  for (const auto& param : request_params) {
    if (!query.empty()) {
      query += '&';
    }
    query += CodecUtil::UrlEncode(param.first);
    if (!param.second.empty()) {
      query += '=';
      query += CodecUtil::UrlEncode(param.second);
    }
  }
  if (!query.empty()) {
    request_url += '?' + query;
  }
  return request_url;
}

std::string TrimHeaderValue(const std::string& value) {
  size_t begin = 0;
  size_t end = value.size();
  while (begin < end &&
         (value[begin] == ' ' || value[begin] == '\t' ||
          value[begin] == '\r' || value[begin] == '\n')) {
    ++begin;
  }
  while (end > begin &&
         (value[end - 1] == ' ' || value[end - 1] == '\t' ||
          value[end - 1] == '\r' || value[end - 1] == '\n')) {
    --end;
  }
  return value.substr(begin, end - begin);
}

std::string Lowercase(std::string value) {
  for (char& character : value) {
    character = static_cast<char>(
        std::tolower(static_cast<unsigned char>(character)));
  }
  return value;
}

struct HeaderContext {
  std::map<std::string, std::string>* headers = nullptr;
};

size_t ParseHeaderCallback(char* buffer, size_t size, size_t item_count,
                           void* user_data) {
  HeaderContext* context = static_cast<HeaderContext*>(user_data);
  const size_t length = size * item_count;
  if (context == nullptr || context->headers == nullptr || length == 0) {
    return length;
  }

  std::string line(buffer, length);
  const size_t colon = line.find(':');
  if (colon == std::string::npos) {
    return length;
  }

  std::string name = TrimHeaderValue(line.substr(0, colon));
  std::string value = TrimHeaderValue(line.substr(colon + 1));
  if (name.empty()) {
    return length;
  }
  if (Lowercase(name) == "etag") {
    name = "ETag";
  }
  context->headers->insert(std::make_pair(name, value));
  if (name == "ETag") {
    (*context->headers)[name] = value;
  }
  return length;
}

size_t ReadCallback(char* buffer, size_t size, size_t item_count,
                    void* user_data) {
  CurlRequestContext* context = static_cast<CurlRequestContext*>(user_data);
  const size_t capacity = size * item_count;
  if (context == nullptr || capacity == 0) {
    return 0;
  }
  if (context->handler && !context->handler->ShouldContinue()) {
    context->callback_cancelled = true;
    return CURL_READFUNC_ABORT;
  }

  size_t read_size = 0;
  try {
    if (context->request_buffer != nullptr) {
      const size_t remaining = context->request_length - context->request_offset;
      read_size = std::min(capacity, remaining);
      if (read_size > 0) {
        std::memcpy(buffer, context->request_buffer + context->request_offset,
                    read_size);
        context->request_offset += read_size;
      }
    } else if (context->request_stream != nullptr) {
      const std::streamsize requested = static_cast<std::streamsize>(
          std::min(capacity,
                   static_cast<size_t>(std::numeric_limits<std::streamsize>::max())));
      context->request_stream->read(buffer, requested);
      const std::streamsize count = context->request_stream->gcount();
      if (count > 0) {
        read_size = static_cast<size_t>(count);
      }
      if (context->request_stream->bad()) {
        context->request_failed = true;
        return CURL_READFUNC_ABORT;
      }
    }
  } catch (...) {
    context->request_failed = true;
    return CURL_READFUNC_ABORT;
  }

  if (read_size > 0) {
    context->sent_bytes += read_size;
    if (context->handler) {
      context->handler->UpdateProgress(read_size);
    }
  }
  return read_size;
}

size_t WriteCallback(char* buffer, size_t size, size_t item_count,
                     void* user_data) {
  CurlRequestContext* context = static_cast<CurlRequestContext*>(user_data);
  const size_t length = size * item_count;
  if (context == nullptr || length == 0) {
    return length;
  }
  if (context->handler && !context->handler->ShouldContinue()) {
    context->callback_cancelled = true;
    return 0;
  }

  try {
    if (context->capture_response) {
      if (context->captured_response == nullptr) {
        context->response_failed = true;
        return 0;
      }
      context->captured_response->append(buffer, length);
    } else if (context->response_stream != nullptr) {
      context->response_stream->write(buffer,
                                      static_cast<std::streamsize>(length));
      if (!*context->response_stream) {
        context->response_failed = true;
        return 0;
      }
    }
  } catch (...) {
    context->response_failed = true;
    return 0;
  }

  context->received_bytes += length;
  if (context->handler && !context->capture_response) {
    context->handler->UpdateProgress(length);
  }
  return length;
}

CURLcode CurlSslContextCallback(CURL* curl, void* ssl_context,
                                void* user_data) {
  (void)curl;
  SslCallbackContext* context = static_cast<SslCallbackContext*>(user_data);
  if (context == nullptr || context->callback == nullptr ||
      !*context->callback) {
    return CURLE_OK;
  }
  try {
    const int callback_result = (*context->callback)(ssl_context,
                                                      context->user_data);
    if (callback_result != 0) {
      context->error = "SSL_Ctx Callback Exception Code: " +
                       std::to_string(callback_result);
      return CURLE_ABORTED_BY_CALLBACK;
    }
  } catch (const std::exception& exception) {
    context->error = "SSL_Ctx Callback Exception: ";
    context->error += exception.what();
    return CURLE_ABORTED_BY_CALLBACK;
  } catch (...) {
    context->error = "SSL_Ctx Callback Exception";
    return CURLE_ABORTED_BY_CALLBACK;
  }
  return CURLE_OK;
}

int64_t GetResponseContentLength(
    const std::map<std::string, std::string>* response_headers) {
  if (response_headers == nullptr) {
    return -1;
  }
  for (const auto& header : *response_headers) {
    if (Lowercase(header.first) == "content-length" && !header.second.empty()) {
      return static_cast<int64_t>(StringUtil::StringToUint64(header.second));
    }
  }
  return -1;
}

std::string FindHeaderValue(
    const std::map<std::string, std::string>& headers,
    const std::string& name) {
  const std::string target = Lowercase(name);
  for (const auto& header : headers) {
    if (Lowercase(header.first) == target) {
      return header.second;
    }
  }
  return "";
}

void LogResponseMessage(const std::map<std::string, std::string>* response_headers,
                        int status_code, const std::string& error_message) {
  std::ostringstream output;
  output << "response header :\n";
  if (response_headers != nullptr) {
    for (const auto& header : *response_headers) {
      output << header.first << ": " << header.second << "\n";
    }
  }
  output << "Send request over, ret=" << status_code;
  if (!error_message.empty()) {
    output << ", error_message=" << error_message;
  }
  SDK_LOG_DBG("%s", output.str().c_str());
}

void PrintRate(std::chrono::time_point<std::chrono::steady_clock> start,
               std::chrono::time_point<std::chrono::steady_clock> end,
               uint64_t copy_size, const std::string& action) {
  const int64_t elapsed_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
  if (elapsed_ms > 1 && copy_size > 100 * 1024) {
    const float rate = (static_cast<float>(copy_size) / 1024.0f / 1024.0f) /
                       (static_cast<float>(elapsed_ms) / 1000.0f);
    SDK_LOG_DBG("%s_size: %" PRIu64 ", time_consumed: %" PRIu64
                " ms, rate: %.2f MB/s",
                action.c_str(), copy_size, static_cast<uint64_t>(elapsed_ms),
                rate);
  }
}

int CheckResponseBodyLength(const std::string& http_method,
                            int64_t expected_length, int64_t actual_length,
                            std::string* error_message) {
  if (http_method != "GET" || expected_length <= 0) {
    return 0;
  }
  SDK_LOG_DBG("Check response body length, content_length=%" PRId64
              ", actual_length=%" PRId64,
              expected_length, actual_length);
  if (expected_length != actual_length) {
    if (error_message != nullptr) {
      *error_message = "response body incomplete: recv-len=" +
                       std::to_string(actual_length) +
                       ", content-length=" +
                       std::to_string(expected_length);
    }
    SDK_LOG_ERR("Check response body fail: %s",
                error_message == nullptr ? "" : error_message->c_str());
    return -1;
  }
  return 0;
}

long ToCurlTimeout(uint64_t timeout_in_ms) {
  const uint64_t max_long =
      static_cast<uint64_t>(std::numeric_limits<long>::max());
  return static_cast<long>(timeout_in_ms > max_long ? max_long
                                                    : timeout_in_ms);
}

int PerformRequest(
    const SharedTransferHandler& handler, const std::string& http_method,
    const std::string& url_str,
    const std::map<std::string, std::string>& request_params,
    const std::map<std::string, std::string>& request_headers,
    std::istream* request_stream, const char* request_buffer,
    size_t request_length, std::ostream* response_stream,
    bool capture_response, std::string* captured_response,
    uint64_t conn_timeout_in_ms, uint64_t recv_timeout_in_ms,
    std::map<std::string, std::string>* response_headers,
    std::string* error_message, bool is_verify_cert,
    const std::string& ca_location, const SSLCtxCallback& ssl_ctx_callback,
    void* user_data, uint64_t* received_bytes) {
  if (error_message != nullptr) {
    error_message->clear();
  }
  if (received_bytes != nullptr) {
    *received_bytes = 0;
  }

  if (EnsureCurlInitialized() != CURLE_OK) {
    if (error_message != nullptr) {
      *error_message = "curl_global_init failed";
    }
    return kHttpStatusNetError;
  }

  // A per-request TLS context callback can install its own credentials, and
  // libcurl does not take it into account when matching an existing connection.
  // Such requests therefore get a private handle instead of a pooled one.
  PooledCurlHandle curl(/*allow_pooled=*/!ssl_ctx_callback);
  if (!curl) {
    if (error_message != nullptr) {
      *error_message = "curl_easy_init failed";
    }
    return kHttpStatusNetError;
  }

  CurlRequestContext context;
  context.handler = handler;
  context.request_stream = request_stream;
  context.request_buffer = request_buffer;
  context.request_length = request_length;
  context.response_stream = response_stream;
  context.captured_response = captured_response;
  context.capture_response = capture_response;
  if (captured_response != nullptr && capture_response) {
    captured_response->clear();
  }

  std::map<std::string, std::string> parsed_response_headers;
  HeaderContext header_context;
  header_context.headers = &parsed_response_headers;

  const std::string request_url = BuildRequestUrl(url_str, request_params);
  char curl_error[CURL_ERROR_SIZE] = {};
  curl_easy_setopt(curl.get(), CURLOPT_ERRORBUFFER, curl_error);
  curl_easy_setopt(curl.get(), CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(curl.get(), CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
  curl_easy_setopt(curl.get(), CURLOPT_URL, request_url.c_str());
  curl_easy_setopt(curl.get(), CURLOPT_CUSTOMREQUEST, http_method.c_str());
  curl_easy_setopt(curl.get(), CURLOPT_HEADERFUNCTION, &ParseHeaderCallback);
  curl_easy_setopt(curl.get(), CURLOPT_HEADERDATA, &header_context);
  curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, &WriteCallback);
  curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &context);

  if (conn_timeout_in_ms > 0) {
    curl_easy_setopt(curl.get(), CURLOPT_CONNECTTIMEOUT_MS,
                    ToCurlTimeout(conn_timeout_in_ms));
  }
  if (recv_timeout_in_ms > 0) {
    curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT_MS,
                    ToCurlTimeout(recv_timeout_in_ms));
  }

  if (http_method == "HEAD") {
    curl_easy_setopt(curl.get(), CURLOPT_NOBODY, 1L);
  }

  if (CosSysConfig::GetKeepAlive()) {
    curl_easy_setopt(curl.get(), CURLOPT_TCP_KEEPALIVE, 1L);
    const int64_t keep_idle = CosSysConfig::GetKeepIdle();
    const int64_t keep_intvl = CosSysConfig::GetKeepIntvl();
    if (keep_idle > 0) {
      curl_easy_setopt(curl.get(), CURLOPT_TCP_KEEPIDLE,
                       static_cast<long>(keep_idle));
    }
    if (keep_intvl > 0) {
      curl_easy_setopt(curl.get(), CURLOPT_TCP_KEEPINTVL,
                       static_cast<long>(keep_intvl));
    }
  }

  curl_slist* raw_headers = nullptr;
  for (const auto& header : request_headers) {
    const std::string line = header.first + ": " + header.second;
    raw_headers = curl_slist_append(raw_headers, line.c_str());
    if (raw_headers == nullptr) {
      if (error_message != nullptr) {
        *error_message = "curl_slist_append failed";
      }
      return kHttpStatusNetError;
    }
  }
  CurlSlist headers(raw_headers);
  if (headers) {
    curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, headers.get());
  }

  const std::string method_upper = StringUtil::StringToUpper(http_method);
  const std::string content_length =
      FindHeaderValue(request_headers, "Content-Length");
  const bool has_known_body = request_buffer != nullptr ||
                              (!content_length.empty() &&
                               StringUtil::StringToUint64(content_length) > 0);
  const bool has_body = request_buffer != nullptr || has_known_body ||
                        (method_upper == "PUT" || method_upper == "POST" ||
                         method_upper == "PATCH");
  if (has_body) {
    curl_easy_setopt(curl.get(), CURLOPT_READFUNCTION, &ReadCallback);
    curl_easy_setopt(curl.get(), CURLOPT_READDATA, &context);
    if (method_upper == "POST") {
      curl_easy_setopt(curl.get(), CURLOPT_POST, 1L);
    } else {
      curl_easy_setopt(curl.get(), CURLOPT_UPLOAD, 1L);
    }

    uint64_t known_length = 0;
    bool has_length = false;
    if (request_buffer != nullptr) {
      known_length = request_length;
      has_length = true;
    } else if (!content_length.empty()) {
      known_length = StringUtil::StringToUint64(content_length);
      has_length = true;
    }
    if (has_length) {
      curl_easy_setopt(curl.get(), CURLOPT_INFILESIZE_LARGE,
                      static_cast<curl_off_t>(known_length));
    }
  }

  curl_easy_setopt(curl.get(), CURLOPT_SSL_VERIFYPEER,
                  is_verify_cert ? 1L : 0L);
  curl_easy_setopt(curl.get(), CURLOPT_SSL_VERIFYHOST,
                  is_verify_cert ? 2L : 0L);
  if (!ca_location.empty()) {
    curl_easy_setopt(curl.get(), CURLOPT_CAINFO, ca_location.c_str());
  }

  SslCallbackContext ssl_context;
  ssl_context.callback = &ssl_ctx_callback;
  ssl_context.user_data = user_data;
  if (ssl_ctx_callback) {
    curl_easy_setopt(curl.get(), CURLOPT_SSL_CTX_FUNCTION,
                    &CurlSslContextCallback);
    curl_easy_setopt(curl.get(), CURLOPT_SSL_CTX_DATA, &ssl_context);
  }

  SDK_LOG_INFO("send request to [%s]", url_str.c_str());
  SDK_LOG_DBG("request=[%s %s]", http_method.c_str(), request_url.c_str());

  const CURLcode curl_result = curl_easy_perform(curl.get());
  curl.SetReusable(CurlHandlePool::IsConnectionReusable(curl_result));

  if (received_bytes != nullptr) {
    *received_bytes = context.received_bytes;
  }
  if (response_headers != nullptr) {
    response_headers->insert(parsed_response_headers.begin(),
                             parsed_response_headers.end());
    if (response_headers->count("Etag") > 0 &&
        response_headers->count("ETag") == 0) {
      (*response_headers)["ETag"] = (*response_headers)["Etag"];
      response_headers->erase("Etag");
    }
  }

  if (curl_result != CURLE_OK) {
    const bool user_cancelled =
        context.callback_cancelled ||
        (handler && !handler->ShouldContinue());
    if (user_cancelled) {
      if (error_message != nullptr) {
        *error_message = "Request canceled by user";
      }
      return kHttpStatusUserCancel;
    }
    if (error_message != nullptr) {
      if (!ssl_context.error.empty()) {
        *error_message = ssl_context.error;
      } else if (context.request_failed) {
        *error_message = "request input stream failed";
      } else if (context.response_failed) {
        *error_message = "response output stream failed";
      } else if (curl_error[0] != '\0') {
        *error_message = curl_error;
      } else {
        *error_message = curl_easy_strerror(curl_result);
      }
    }
    LogResponseMessage(response_headers, kHttpStatusNetError,
                       error_message == nullptr ? "" : *error_message);
    return kHttpStatusNetError;
  }

  long status_code = 0;
  curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &status_code);
  LogResponseMessage(response_headers, static_cast<int>(status_code), "");
  return static_cast<int>(status_code);
}

int CopyCapturedResponse(const SharedTransferHandler& handler,
                         const std::string& response_body,
                         std::ostream* response_stream,
                         uint64_t* copied_bytes, std::string* error_message) {
  if (copied_bytes != nullptr) {
    *copied_bytes = 0;
  }
  if (response_stream == nullptr) {
    return 0;
  }
  try {
    const std::streamsize copied = HandleStreamCopier::handleCopyStream(
        handler, response_body.data(), response_body.size(), *response_stream);
    if (copied_bytes != nullptr) {
      *copied_bytes = static_cast<uint64_t>(copied);
    }
    return 0;
  } catch (const UserCancelException&) {
    if (error_message != nullptr) {
      *error_message = "Request canceled by user";
    }
    return kHttpStatusUserCancel;
  } catch (const std::exception& exception) {
    if (error_message != nullptr) {
      *error_message = exception.what();
    }
    return kHttpStatusNetError;
  }
}

}  // namespace

int HttpSender::SendRequest(
    const SharedTransferHandler& handler, const std::string& http_method,
    const std::string& url_str,
    const std::map<std::string, std::string>& req_params,
    const std::map<std::string, std::string>& req_headers,
    const std::string& req_body, uint64_t conn_timeout_in_ms,
    uint64_t recv_timeout_in_ms,
    std::map<std::string, std::string>* resp_headers, std::string* resp_body,
    std::string* err_msg, bool is_check_md5, bool is_verify_cert,
    const std::string& ca_location, const SSLCtxCallback& ssl_ctx_cb,
    void* user_data) {
  std::istringstream input(req_body);
  std::ostringstream output;
  const int result = SendRequest(
      handler, http_method, url_str, req_params, req_headers, input,
      conn_timeout_in_ms, recv_timeout_in_ms, resp_headers, output, err_msg,
      is_check_md5, is_verify_cert, ca_location, ssl_ctx_cb, user_data);
  if (resp_body != nullptr) {
    *resp_body = output.str();
  }
  return result;
}

int HttpSender::SendRequest(
    const SharedTransferHandler& handler, const std::string& http_method,
    const std::string& url_str,
    const std::map<std::string, std::string>& req_params,
    const std::map<std::string, std::string>& req_headers, std::istream& is,
    uint64_t conn_timeout_in_ms, uint64_t recv_timeout_in_ms,
    std::map<std::string, std::string>* resp_headers, std::string* resp_body,
    std::string* err_msg, bool is_check_md5, bool is_verify_cert,
    const std::string& ca_location, const SSLCtxCallback& ssl_ctx_cb,
    void* user_data) {
  std::ostringstream output;
  const int result = SendRequest(
      handler, http_method, url_str, req_params, req_headers, is,
      conn_timeout_in_ms, recv_timeout_in_ms, resp_headers, output, err_msg,
      is_check_md5, is_verify_cert, ca_location, ssl_ctx_cb, user_data);
  if (resp_body != nullptr) {
    *resp_body = output.str();
  }
  return result;
}

int HttpSender::SendRequest(
    const SharedTransferHandler& handler, const std::string& http_method,
    const std::string& url_str,
    const std::map<std::string, std::string>& req_params,
    const std::map<std::string, std::string>& req_headers, std::istream& is,
    uint64_t conn_timeout_in_ms, uint64_t recv_timeout_in_ms,
    std::map<std::string, std::string>* resp_headers, std::ostream& resp_stream,
    std::string* err_msg, bool is_check_md5, bool is_verify_cert,
    const std::string& ca_location, const SSLCtxCallback& ssl_ctx_cb,
    void* user_data, const char* req_body_buf, size_t req_body_len) {
  std::string response_body;
  const bool capture_response = is_check_md5;
  uint64_t received_bytes = 0;
  int status_code = PerformRequest(
      handler, http_method, url_str, req_params, req_headers, &is,
      req_body_buf, req_body_len, &resp_stream, capture_response,
      &response_body, conn_timeout_in_ms, recv_timeout_in_ms, resp_headers,
      err_msg, is_verify_cert, ca_location, ssl_ctx_cb, user_data,
      &received_bytes);
  if (status_code < 0) {
    return status_code;
  }

  const auto start = std::chrono::steady_clock::now();
  uint64_t copied_bytes = received_bytes;
  if (capture_response) {
    const int copy_result = CopyCapturedResponse(
        handler, response_body, &resp_stream, &copied_bytes, err_msg);
    if (copy_result < 0) {
      return copy_result;
    }

    std::string etag;
    if (resp_headers != nullptr) {
      auto etag_iterator = resp_headers->find("ETag");
      if (etag_iterator != resp_headers->end()) {
        etag = StringUtil::Trim(etag_iterator->second, "\"");
      }
    }
    if (status_code >= 200 && status_code < 300 &&
        !StringUtil::IsV4ETag(etag) &&
        !StringUtil::IsMultipartUploadETag(etag)) {
      const std::string md5 = internal::Md5Hex(response_body);
      if (etag != md5) {
        if (err_msg != nullptr) {
          *err_msg = "Md5 of response body is not equal to the etag in the "
                     "header. Body Md5= " +
                     md5 + ", etag=" + etag;
        }
        SDK_LOG_ERR("Check Md5 fail, %s",
                    err_msg == nullptr ? "" : err_msg->c_str());
        status_code = kHttpStatusNetError;
      }
    }
  }
  const auto end = std::chrono::steady_clock::now();
  PrintRate(start, end, copied_bytes, "recv");

  const int64_t content_length = GetResponseContentLength(resp_headers);
  if (CheckResponseBodyLength(http_method, content_length,
                              static_cast<int64_t>(copied_bytes), err_msg) < 0) {
    status_code = kHttpStatusNetError;
  }
  return status_code;
}

int HttpSender::SendRequest(
    const SharedTransferHandler& handler, const std::string& http_method,
    const std::string& url_str,
    const std::map<std::string, std::string>& req_params,
    const std::map<std::string, std::string>& req_headers,
    const std::string& req_body, uint64_t conn_timeout_in_ms,
    uint64_t recv_timeout_in_ms,
    std::map<std::string, std::string>* resp_headers,
    std::string* xml_err_str, std::ostream& resp_stream, std::string* err_msg,
    uint64_t* real_byte, bool is_check_md5, bool is_verify_cert,
    const std::string& ca_location, const SSLCtxCallback& ssl_ctx_cb,
    void* user_data) {
  if (real_byte != nullptr) {
    *real_byte = 0;
  }
  std::istringstream input(req_body);
  std::string response_body;
  uint64_t received_bytes = 0;
  int status_code = PerformRequest(
      handler, http_method, url_str, req_params, req_headers, &input, nullptr,
      0, nullptr, true, &response_body, conn_timeout_in_ms,
      recv_timeout_in_ms, resp_headers, err_msg, is_verify_cert, ca_location,
      ssl_ctx_cb, user_data, &received_bytes);
  if (real_byte != nullptr) {
    *real_byte = received_bytes;
  }
  if (status_code < 0) {
    return status_code;
  }

  if (status_code < 200 || status_code > 299) {
    if (xml_err_str != nullptr) {
      *xml_err_str = response_body;
    }
    return status_code;
  }

  const int64_t content_length = GetResponseContentLength(resp_headers);
  if (handler && content_length > 0) {
    handler->SetTotalSize(static_cast<uint64_t>(content_length));
  }

  const auto start = std::chrono::steady_clock::now();
  uint64_t copied_bytes = 0;
  const int copy_result = CopyCapturedResponse(
      handler, response_body, &resp_stream, &copied_bytes, err_msg);
  if (real_byte != nullptr) {
    *real_byte = copied_bytes;
  }
  if (copy_result < 0) {
    return copy_result;
  }
  const auto end = std::chrono::steady_clock::now();
  PrintRate(start, end, copied_bytes, "recv");

  if (is_check_md5 && resp_headers != nullptr) {
    std::string etag;
    auto etag_iterator = resp_headers->find("ETag");
    if (etag_iterator != resp_headers->end()) {
      etag = StringUtil::Trim(etag_iterator->second, "\"");
    }
    if (!StringUtil::IsV4ETag(etag) &&
        !StringUtil::IsMultipartUploadETag(etag)) {
      const std::string md5 = internal::Md5Hex(response_body);
      if (etag != md5) {
        if (err_msg != nullptr) {
          *err_msg = "Md5 of response body is not equal to the etag in the "
                     "header. Body Md5= " +
                     md5 + ", etag=" + etag + ", recv-len=" +
                     StringUtil::Uint64ToString(copied_bytes) +
                     ", content-length=" +
                     std::to_string(content_length);
        }
        status_code = kHttpStatusNetError;
      }
    }
  }
  if (CheckResponseBodyLength(http_method, content_length,
                              static_cast<int64_t>(copied_bytes), err_msg) < 0) {
    status_code = kHttpStatusNetError;
  }
  return status_code;
}

}  // namespace qcloud_cos
