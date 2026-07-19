#pragma once

#include <string>

namespace osp {

std::string trim_ascii(const std::string& input);
std::string normalize_os_host(const std::string& input);
std::string normalize_md5_hex(const std::string& input);
bool is_valid_md5_hex(const std::string& input);
bool has_provisioning_config(const std::string& ssid,
                             const std::string& host,
                             const std::string& pw_md5);

}  // namespace osp
