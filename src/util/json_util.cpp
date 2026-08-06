#include "util/json_util.h"

#include <iomanip>
#include <sstream>

namespace qcloud_cos {

std::string JsonUtil::EscapeJsonString(const std::string& value) {
  std::ostringstream output;
  output << '"';
  for (unsigned char c : value) {
    switch (c) {
      case '"': output << "\\\""; break;
      case '\\': output << "\\\\"; break;
      case '\b': output << "\\b"; break;
      case '\f': output << "\\f"; break;
      case '\n': output << "\\n"; break;
      case '\r': output << "\\r"; break;
      case '\t': output << "\\t"; break;
      default:
        if (c < 0x20) {
          output << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                 << static_cast<unsigned int>(c) << std::dec
                 << std::setfill('0');
        } else {
          output.put(static_cast<char>(c));
        }
    }
  }
  output << '"';
  return output.str();
}

}  // namespace qcloud_cos
