#include "cos_config.h"

#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>

#include "cos_sys_config.h"
#include "internal/json_value.h"
#include "util/string_util.h"
#include "util/illegal_intercept.h"

namespace qcloud_cos {
CosConfig::CosConfig(const std::string& config_file)
    : m_app_id(0),
      m_access_key(""),
      m_secret_key(""),
      m_region(""),
      m_tmp_token(""),
      m_set_intranet_once(false),
      m_is_use_intranet(false),
      m_intranet_addr(""),
      m_dest_domain(""),
      m_is_domain_same_to_host(false),
      m_is_domain_same_to_host_enable(false),
      m_config_parsed(false),
      m_max_retry_times(COS_DEFAULT_MAX_RETRY_TIMES),
      m_retry_interval_ms(COS_DEFAULT_RETRY_INTERVAL_MS),
      m_enable_checkpoint(false),
      m_checkpoint_dir("") {
  if (InitConf(config_file)) {
    m_config_parsed = true;
  }
}

bool CosConfig::InitConf(const std::string& config_file) {
  std::ifstream ifs(config_file.c_str(), std::ios::in);
  if (!ifs || !ifs.is_open()) {
    std::cerr << "failed to open config file " << config_file << std::endl;
    return false;
  }

  std::ostringstream config_stream;
  config_stream << ifs.rdbuf();
  internal::JsonValue object;
  std::string parse_error;
  if (!internal::JsonValue::Parse(config_stream.str(), &object, &parse_error) ||
      !object.IsObject()) {
    std::cerr << "failed to parse config file, " << config_file;
    if (!parse_error.empty()) std::cerr << ", " << parse_error;
    std::cerr << std::endl;
    return false;
  }

  auto get_string = [&object](const std::string& key, std::string* value) {
    const internal::JsonValue* field = object.Find(key);
    if (!field) return false;
    if (!field->AsString(value)) {
      std::cerr << "failed to parse config file, " << key
                << " should be string" << std::endl;
      return false;
    }
    return true;
  };
  auto get_integer = [&object](const std::string& key, uint64_t* value) {
    const internal::JsonValue* field = object.Find(key);
    if (!field) return false;
    if (!field->AsUInt64(value)) {
      std::cerr << "failed to parse config file, " << key
                << " should be unsigned integer" << std::endl;
      return false;
    }
    return true;
  };
  auto get_bool = [&object](const std::string& key, bool* value) {
    const internal::JsonValue* field = object.Find(key);
    if (!field) return false;
    if (!field->AsBool(value)) {
      std::cerr << "failed to parse config file, " << key
                << " should be boolean" << std::endl;
      return false;
    }
    return true;
  };

  get_integer("AppID", &m_app_id);
  get_string("AccessKey", &m_access_key);
  get_string("SecretId", &m_access_key);
  get_string("SecretKey", &m_secret_key);
  if (m_access_key.empty() || m_secret_key.empty()) {
    std::cerr << "warnning, access_key or serete_key not exists" << std::endl;
  }
  m_access_key = StringUtil::Trim(m_access_key);
  m_secret_key = StringUtil::Trim(m_secret_key);
  //设置cos区域和下载域名:cos,cdn,innercos,自定义,默认:cos
  get_string("Region", &m_region);
  m_region = StringUtil::Trim(m_region);

  get_integer("RetryIntervalMs", &m_retry_interval_ms);

  get_integer("MaxRetryTimes", &m_max_retry_times);

  uint64_t integer_value;

  //设置签名超时时间,单位:秒
  if (get_integer("SignExpiredTime", &integer_value)) {
    CosSysConfig::SetAuthExpiredTime(integer_value);
  }

  //设置连接超时时间,单位:毫秒
  if (get_integer("ConnectTimeoutInms", &integer_value)) {
    CosSysConfig::SetConnTimeoutInms(integer_value);
  }

  //设置接收超时时间,单位:毫秒
  if (get_integer("ReceiveTimeoutInms", &integer_value)) {
    CosSysConfig::SetRecvTimeoutInms(integer_value);
  }

  //设置上传分片大小,默认:10M
  if (get_integer("UploadPartSize", &integer_value)) {
    CosSysConfig::SetUploadPartSize(integer_value);
  }

  //设置单文件分片并发上传的线程池大小
  if (get_integer("UploadThreadPoolSize", &integer_value)) {
    CosSysConfig::SetUploadThreadPoolSize((unsigned)integer_value);
  }

  //异步上传下载的线程池大小
  if (get_integer("AsynThreadPoolSize", &integer_value)) {
    CosSysConfig::SetAsynThreadPoolSize((unsigned)integer_value);
  }

  //设置log输出,0:不输出, 1:屏幕,2:syslog,默认:0
  if (get_integer("LogoutType", &integer_value)) {
    CosSysConfig::SetLogOutType((LOG_OUT_TYPE)integer_value);
  }

  // 设置日志级别
  if (get_integer("LogLevel", &integer_value)) {
    CosSysConfig::SetLogLevel((LOG_LEVEL)integer_value);
  }

  if (get_integer("DownloadThreadPoolSize", &integer_value)) {
    CosSysConfig::SetDownThreadPoolSize((unsigned)integer_value);
  }

  if (get_integer("DownloadSliceSize", &integer_value)) {
    CosSysConfig::SetDownSliceSize((unsigned)integer_value);
  }

  bool bool_value;
  // 长连接 / CURL 句柄池(连接复用)
  if (get_bool("keepalive_mode", &bool_value)) {
    CosSysConfig::SetKeepAlive(bool_value);
  }
  if (get_integer("keepalive_idle_time", &integer_value)) {
    CosSysConfig::SetKeepIdle(static_cast<int64_t>(integer_value));
  }
  if (get_integer("keepalive_interval_time", &integer_value)) {
    CosSysConfig::SetKeepIntvl(static_cast<int64_t>(integer_value));
  }
  if (get_integer("CurlHandlePoolSize", &integer_value)) {
    CosSysConfig::SetCurlHandlePoolSize(static_cast<unsigned>(integer_value));
  }
  if (get_bool("IsCheckMd5", &bool_value)) {
    CosSysConfig::SetCheckMd5(bool_value);
  }

  std::string str_value;
  if (get_string("DestDomain", &str_value)) {
    CosSysConfig::SetDestDomain(str_value);
    m_dest_domain = str_value;
  }

  if (get_bool("IsDomainSameToHost", &bool_value)) {
    CosSysConfig::SetDomainSameToHost(bool_value);
  }

  if (get_bool("IsUseIntranet", &bool_value)) {
    CosSysConfig::SetIsUseIntranet(bool_value);
    m_is_use_intranet = bool_value;
    m_set_intranet_once = true;
  }

  if (get_string("IntranetAddr", &str_value)) {
    CosSysConfig::SetIntranetAddr(str_value);
    m_intranet_addr = str_value;
    m_set_intranet_once = true;
  }

  CosSysConfig::PrintValue();
  return true;
}

uint64_t CosConfig::GetAppId() const { return m_app_id; }

std::string CosConfig::GetAccessKey() const {
  std::lock_guard<std::mutex> lock(m_lock);
  std::string ak = m_access_key;
  return ak;
}

std::string CosConfig::GetSecretKey() const {
  std::lock_guard<std::mutex> lock(m_lock);
  std::string sk = m_secret_key;
  return sk;
}

std::string CosConfig::GetRegion() const { return m_region; }

std::string CosConfig::GetTmpToken() const {
  std::lock_guard<std::mutex> lock(m_lock);
  std::string token = m_tmp_token;
  return token;
}

uint64_t CosConfig::GetMaxRetryTimes() const {
  return m_max_retry_times;
}

void CosConfig::SetMaxRetryTimes(uint64_t max_retry_count) {
  m_max_retry_times = max_retry_count;
}

uint64_t CosConfig::GetRetryIntervalMs() const {
  return m_retry_interval_ms;
}

void CosConfig::SetRetryIntervalMs(uint64_t retry_interval_ms) {
  m_retry_interval_ms = retry_interval_ms;
}

void CosConfig::SetConfigCredentail(const std::string& access_key,
                                    const std::string& secret_key,
                                    const std::string& tmp_token) {
  std::lock_guard<std::mutex> lock(m_lock);
  m_access_key = access_key;
  m_secret_key = secret_key;
  m_tmp_token = tmp_token;
}

void CosConfig::SetIsUseIntranetAddr(bool is_use_intranet) {
  CosSysConfig::SetIsUseIntranet(is_use_intranet);
  m_is_use_intranet = is_use_intranet;

  m_set_intranet_once = true;
}

bool CosConfig::IsUseIntranet() {
  return m_is_use_intranet;
}

void CosConfig::SetIntranetAddr(const std::string& intranet_addr) {
  CosSysConfig::SetIntranetAddr(intranet_addr);
  m_intranet_addr = intranet_addr;

  m_set_intranet_once = true;
}

std::string CosConfig::GetIntranetAddr() {
  return m_intranet_addr;
}

void CosConfig::SetDestDomain(const std::string& domain) {
  CosSysConfig::SetDestDomain(domain);
  m_dest_domain = domain;
}

const std::string& CosConfig::GetDestDomain() const {
  return m_dest_domain;
}

bool CosConfig::IsDomainSameToHost() const {
  return m_is_domain_same_to_host;
}

void CosConfig::SetDomainSameToHost(bool is_domain_same_to_host) {
  m_is_domain_same_to_host = is_domain_same_to_host;
  m_is_domain_same_to_host_enable = true;
}

bool CosConfig::IsDomainSameToHostEnable() const {
  return m_is_domain_same_to_host_enable;
}

void CosConfig::SetLogCallback(const LogCallback log_callback) {
  CosSysConfig::SetLogCallback(log_callback);
}

bool CosConfig::CheckRegion() {
  // 检查 region 是否符合规范
  if (m_region.empty() || !IllegalIntercept::isAlnum(m_region.front()) || !IllegalIntercept::isAlnum(m_region.back())) {
    return false;
  }
  for (size_t i = 1; i < m_region.size() - 1; ++i) {
    char c = m_region[i];
    if (!IllegalIntercept::isAlnum(c) && c != '.' && c != '-') {
      return false;
    }
  }
  return true;
}

}  // namespace qcloud_cos
