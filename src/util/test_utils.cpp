#include "util/test_utils.h"

#include <stdio.h>
#include <stdlib.h>

#include <fstream>
#include <sstream>
#if defined(__linux__)
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#include "internal/crypto_util.h"

namespace qcloud_cos {

void TestUtils::WriteStringtoFile(const std::string& file,
                                  const std::string& str) {
  std::fstream fs(file, std::ios::out | std::ios::binary | std::ios::trunc);
  fs << str;
  fs.close();
}
void TestUtils::WriteRandomDatatoFile(const std::string& file, unsigned len) {
  std::fstream fs(file, std::ios::out | std::ios::binary | std::ios::trunc);
  fs << TestUtils::GetRandomString(len);
  fs.close();
}

void TestUtils::RemoveFile(const std::string& file) { ::remove(file.c_str()); }

std::string TestUtils::GetRandomString(unsigned len) {
  static const char alphanum[] =
      "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmopqrstuvwxyz";
  std::stringstream ss;
  for (unsigned i = 0; i < len; ++i) {
    ss << alphanum[rand() % (sizeof(alphanum) - 1)];
  }
  return ss.str();
}

std::string TestUtils::CalcFileMd5(const std::string& file) {
  std::ifstream ifs(file, std::ios::in | std::ios::binary);
  if (!ifs.is_open()) return "";
  return internal::Md5Hex(ifs);
}

std::string TestUtils::CalcStreamMd5(std::istream& is) {
  return internal::Md5Hex(is);
}

std::string TestUtils::CalcStringMd5(const std::string& str) {
  return internal::Md5Hex(str);
}

std::string TestUtils::CalcStreamSHA1(std::istream& is) {
  return internal::Sha1Hex(is);
}

std::string TestUtils::GetEnvVar(const std::string& env_var_name) {
  char const* tmp = getenv(env_var_name.c_str());
  if (tmp == NULL) {
    return "NOT_EXIST_ENV_" + env_var_name;
  }

  return std::string(tmp);
}
#if defined(__linux__)
bool TestUtils::IsDirectoryExists(const std::string& path) {
  struct stat info;
  if (0 == stat(path.c_str(), &info) && info.st_mode & S_IFDIR) {
    return true;
  } else {
    return false;
  }
}
bool TestUtils::MakeDirectory(const std::string& path) {
  if (0 == mkdir(path.c_str(), 0775)) {
    return true;
  } else {
    return false;
  }
}
bool TestUtils::RemoveDirectory(const std::string& path) {
  if (0 == rmdir(path.c_str())) {
    return true;
  } else {
    return false;
  }
}
#endif
}  // namespace qcloud_cos
