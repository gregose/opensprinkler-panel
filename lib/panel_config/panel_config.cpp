#include "panel_config.h"

#include <algorithm>
#include <cctype>

namespace osp {

std::string trim_ascii(const std::string& input) {
  const auto first = std::find_if_not(input.begin(), input.end(), [](unsigned char ch) {
    return std::isspace(ch) != 0;
  });
  if (first == input.end()) return "";

  const auto last = std::find_if_not(input.rbegin(), input.rend(), [](unsigned char ch) {
    return std::isspace(ch) != 0;
  }).base();
  return std::string(first, last);
}

static bool starts_with_case_insensitive(const std::string& input, const char* prefix) {
  for (size_t i = 0; prefix[i] != '\0'; ++i) {
    if (i >= input.size()) return false;
    if (std::tolower(static_cast<unsigned char>(input[i])) !=
        std::tolower(static_cast<unsigned char>(prefix[i]))) {
      return false;
    }
  }
  return true;
}

std::string normalize_os_host(const std::string& input) {
  std::string out = trim_ascii(input);
  if (starts_with_case_insensitive(out, "http://")) out.erase(0, 7);
  else if (starts_with_case_insensitive(out, "https://")) out.erase(0, 8);
  while (!out.empty() && out.back() == '/') out.pop_back();
  return out;
}

std::string normalize_md5_hex(const std::string& input) {
  std::string out = trim_ascii(input);
  std::transform(out.begin(), out.end(), out.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return out;
}

bool is_valid_md5_hex(const std::string& input) {
  const std::string normalized = normalize_md5_hex(input);
  if (normalized.size() != 32) return false;
  return std::all_of(normalized.begin(), normalized.end(), [](unsigned char ch) {
    return std::isxdigit(ch) != 0;
  });
}

bool has_provisioning_config(const std::string& ssid,
                             const std::string& host,
                             const std::string& pw_md5) {
  return !trim_ascii(ssid).empty() &&
         !normalize_os_host(host).empty() &&
         is_valid_md5_hex(pw_md5);
}

}  // namespace osp
