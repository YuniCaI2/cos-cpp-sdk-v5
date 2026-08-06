#ifndef COS_CPP_SDK_V5_SRC_INTERNAL_CRYPTO_UTIL_H
#define COS_CPP_SDK_V5_SRC_INTERNAL_CRYPTO_UTIL_H

#include <istream>
#include <string>

namespace qcloud_cos {
namespace internal {

std::string Md5Raw(const std::string& input);
std::string Md5Hex(const std::string& input);
std::string Md5Hex(std::istream& input);
std::string HmacSha1Hex(const std::string& input, const std::string& key);
std::string Sha1Hex(std::istream& input);

}  // namespace internal
}  // namespace qcloud_cos

#endif  // COS_CPP_SDK_V5_SRC_INTERNAL_CRYPTO_UTIL_H
