#ifndef COS_CPP_SDK_V5_INCLUDE_UTIL_JSON_UTIL_H_
#define COS_CPP_SDK_V5_INCLUDE_UTIL_JSON_UTIL_H_

#include <stdint.h>

#include <string>

namespace qcloud_cos {

class JsonUtil {
 public:
  // Escape a UTF-8 string for use as a JSON string value.  This keeps JSON
  // generation in public request headers independent of a third-party JSON
  // library.
  static std::string EscapeJsonString(const std::string& value);

  /**
   * @brief 从 JSON 对象中提取字符串字段值（静默方式）
   *
   * @param json_object JSON 对象指针
   * @param key 字段键名
   * @param value 输出参数，存储提取的字符串值
   * @return true 字段存在且为字符串类型，提取成功
   * @return false 字段不存在或不是字符串类型
   */
  // Source-compatible templates for applications that used the old Poco
  // helper directly.  The JSON type is deduced only when an application
  // explicitly calls one of these functions; Poco is not part of the SDK
  // public header dependency graph anymore.
  template <typename JsonObjectPtr>
  static bool GetStringValue(const JsonObjectPtr& json_object,
                             const std::string& key,
                             std::string* value) {
    if (!json_object || !value || !json_object->has(key)) return false;
    const auto json_value = json_object->get(key);
    if (!json_value.isString()) return false;
    *value = json_value.template convert<std::string>();
    return true;
  }

  /**
   * @brief 从 JSON 对象中提取整数字段值（静默方式）
   *
   * @param json_object JSON 对象指针
   * @param key 字段键名
   * @param value 输出参数，存储提取的整数值
   * @return true 字段存在且为整数类型，提取成功
   * @return false 字段不存在或不是整数类型
   */
  template <typename JsonObjectPtr>
  static bool GetIntegerValue(const JsonObjectPtr& json_object,
                              const std::string& key,
                              uint64_t* value) {
    if (!json_object || !value || !json_object->has(key)) return false;
    const auto json_value = json_object->get(key);
    if (!json_value.isInteger()) return false;
    const int64_t parsed = json_value.template convert<int64_t>();
    if (parsed < 0) return false;
    *value = static_cast<uint64_t>(parsed);
    return true;
  }

  /**
   * @brief 从 JSON 对象中提取布尔字段值（静默方式）
   *
   * @param json_object JSON 对象指针
   * @param key 字段键名
   * @param value 输出参数，存储提取的布尔值
   * @return true 字段存在且为布尔类型，提取成功
   * @return false 字段不存在或不是布尔类型
   */
  template <typename JsonObjectPtr>
  static bool GetBoolValue(const JsonObjectPtr& json_object,
                           const std::string& key,
                           bool* value) {
    if (!json_object || !value || !json_object->has(key)) return false;
    const auto json_value = json_object->get(key);
    if (!json_value.isBoolean()) return false;
    *value = json_value.template convert<bool>();
    return true;
  }
};

}  // namespace qcloud_cos

#endif  // COS_CPP_SDK_V5_INCLUDE_UTIL_JSON_UTIL_H_
