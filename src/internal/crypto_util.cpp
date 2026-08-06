#include "internal/crypto_util.h"

#include <openssl/evp.h>
#include <openssl/hmac.h>

#include <array>
#include <stdexcept>

namespace qcloud_cos {
namespace internal {
namespace {

std::string ToHex(const unsigned char* data, unsigned int size) {
  static const char digits[] = "0123456789abcdef";
  std::string result;
  result.reserve(size * 2);
  for (unsigned int i = 0; i < size; ++i) {
    result.push_back(digits[(data[i] >> 4) & 0x0f]);
    result.push_back(digits[data[i] & 0x0f]);
  }
  return result;
}

std::string DigestStream(std::istream& input, const EVP_MD* algorithm) {
  EVP_MD_CTX* context = EVP_MD_CTX_new();
  if (!context) throw std::runtime_error("failed to allocate OpenSSL digest context");

  std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
  unsigned int digest_size = 0;
  bool success = EVP_DigestInit_ex(context, algorithm, nullptr) == 1;
  std::array<char, 64 * 1024> buffer{};
  while (success && input.good()) {
    input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const std::streamsize count = input.gcount();
    if (count > 0) {
      success = EVP_DigestUpdate(context, buffer.data(), static_cast<size_t>(count)) == 1;
    }
  }
  if (success) {
    success = EVP_DigestFinal_ex(context, digest.data(), &digest_size) == 1;
  }
  EVP_MD_CTX_free(context);
  if (!success) throw std::runtime_error("OpenSSL digest operation failed");
  return ToHex(digest.data(), digest_size);
}

}  // namespace

std::string Md5Raw(const std::string& input) {
  std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
  unsigned int digest_size = 0;
  if (EVP_Digest(input.data(), input.size(), digest.data(), &digest_size,
                EVP_md5(), nullptr) != 1) {
    throw std::runtime_error("OpenSSL MD5 operation failed");
  }
  return std::string(reinterpret_cast<const char*>(digest.data()), digest_size);
}

std::string Md5Hex(const std::string& input) {
  const std::string raw = Md5Raw(input);
  return ToHex(reinterpret_cast<const unsigned char*>(raw.data()),
               static_cast<unsigned int>(raw.size()));
}

std::string Md5Hex(std::istream& input) {
  return DigestStream(input, EVP_md5());
}

std::string HmacSha1Hex(const std::string& input, const std::string& key) {
  std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
  unsigned int digest_size = 0;
  if (!HMAC(EVP_sha1(), key.data(), static_cast<int>(key.size()),
            reinterpret_cast<const unsigned char*>(input.data()), input.size(),
            digest.data(), &digest_size)) {
    throw std::runtime_error("OpenSSL HMAC-SHA1 operation failed");
  }
  return ToHex(digest.data(), digest_size);
}

std::string Sha1Hex(std::istream& input) {
  return DigestStream(input, EVP_sha1());
}

}  // namespace internal
}  // namespace qcloud_cos
