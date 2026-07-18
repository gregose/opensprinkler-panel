#include "os_client.h"

#include <ArduinoJson.h>

#include <cstdio>
#include <cstring>

namespace osp {

// ---------------------------------------------------------------------------
// Portable RFC 1321 MD5 — no platform dependency.
// Adapted from the reference implementation (public domain).
// ---------------------------------------------------------------------------
namespace {

using u32 = uint32_t;

static const u32 kT[64] = {
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a,
    0xa8304613, 0xfd469501, 0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be,
    0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821, 0xf61e2562, 0xc040b340,
    0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
    0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8,
    0x676f02d9, 0x8d2a4c8a, 0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c,
    0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70, 0x289b7ec6, 0xeaa127fa,
    0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
    0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92,
    0xffeff47d, 0x85845dd1, 0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1,
    0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391,
};

static inline u32 left_rotate(u32 x, int n) {
  return (x << n) | (x >> (32 - n));
}

void md5_transform(u32 state[4], const uint8_t block[64]) {
  u32 a = state[0], b = state[1], c = state[2], d = state[3];
  u32 M[16];
  for (int i = 0; i < 16; ++i) {
    M[i] = static_cast<u32>(block[i * 4]) |
           (static_cast<u32>(block[i * 4 + 1]) << 8) |
           (static_cast<u32>(block[i * 4 + 2]) << 16) |
           (static_cast<u32>(block[i * 4 + 3]) << 24);
  }

  // Round 1 (F)
  static const int s1[4] = {7, 12, 17, 22};
  for (int i = 0; i < 16; ++i) {
    u32 f = (b & c) | (~b & d);
    u32 g = i;
    u32 tmp = d;
    d = c;
    c = b;
    b = b + left_rotate(a + f + M[g] + kT[i], s1[i & 3]);
    a = tmp;
  }

  // Round 2 (G)
  static const int s2[4] = {5, 9, 14, 20};
  for (int i = 0; i < 16; ++i) {
    u32 f = (d & b) | (~d & c);
    u32 g = (5 * i + 1) % 16;
    u32 tmp = d;
    d = c;
    c = b;
    b = b + left_rotate(a + f + M[g] + kT[16 + i], s2[i & 3]);
    a = tmp;
  }

  // Round 3 (H)
  static const int s3[4] = {4, 11, 16, 23};
  for (int i = 0; i < 16; ++i) {
    u32 f = b ^ c ^ d;
    u32 g = (3 * i + 5) % 16;
    u32 tmp = d;
    d = c;
    c = b;
    b = b + left_rotate(a + f + M[g] + kT[32 + i], s3[i & 3]);
    a = tmp;
  }

  // Round 4 (I)
  static const int s4[4] = {6, 10, 15, 21};
  for (int i = 0; i < 16; ++i) {
    u32 f = c ^ (b | ~d);
    u32 g = (7 * i) % 16;
    u32 tmp = d;
    d = c;
    c = b;
    b = b + left_rotate(a + f + M[g] + kT[48 + i], s4[i & 3]);
    a = tmp;
  }

  state[0] += a;
  state[1] += b;
  state[2] += c;
  state[3] += d;
}

}  // anonymous namespace

std::string md5_hex(const std::string& input) {
  const uint8_t* msg = reinterpret_cast<const uint8_t*>(input.data());
  const size_t len = input.size();

  u32 state[4] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476};

  // Process complete 64-byte blocks.
  size_t i = 0;
  for (; i + 64 <= len; i += 64) {
    md5_transform(state, msg + i);
  }

  // Padding.
  uint8_t buf[128] = {};
  const size_t rem = len - i;
  memcpy(buf, msg + i, rem);
  buf[rem] = 0x80;

  // Length in bits (little-endian u64) in the final 8 bytes.
  const uint64_t bit_len = static_cast<uint64_t>(len) * 8;
  size_t pad_end = (rem < 56) ? 64 : 128;
  buf[pad_end - 8] = static_cast<uint8_t>(bit_len);
  buf[pad_end - 7] = static_cast<uint8_t>(bit_len >> 8);
  buf[pad_end - 6] = static_cast<uint8_t>(bit_len >> 16);
  buf[pad_end - 5] = static_cast<uint8_t>(bit_len >> 24);
  buf[pad_end - 4] = static_cast<uint8_t>(bit_len >> 32);
  buf[pad_end - 3] = static_cast<uint8_t>(bit_len >> 40);
  buf[pad_end - 2] = static_cast<uint8_t>(bit_len >> 48);
  buf[pad_end - 1] = static_cast<uint8_t>(bit_len >> 56);

  md5_transform(state, buf);
  if (pad_end == 128) md5_transform(state, buf + 64);

  // Serialise state as little-endian hex.
  char hex[33];
  for (int j = 0; j < 4; ++j) {
    snprintf(hex + j * 8, 9, "%02x%02x%02x%02x",
             state[j] & 0xFF, (state[j] >> 8) & 0xFF,
             (state[j] >> 16) & 0xFF, (state[j] >> 24) & 0xFF);
  }
  return std::string(hex, 32);
}

// ---------------------------------------------------------------------------
// URL builders
// ---------------------------------------------------------------------------

std::string build_jn_url(const std::string& host, const std::string& pw) {
  return host + "/jn?pw=" + pw;
}

std::string build_jc_url(const std::string& host, const std::string& pw) {
  return host + "/jc?pw=" + pw;
}

std::string build_cm_url(const std::string& host, const std::string& pw,
                         int sid, bool en, int t_sec) {
  char buf[256];
  if (en) {
    snprintf(buf, sizeof(buf), "/cm?pw=%s&sid=%d&en=1&t=%d",
             pw.c_str(), sid, t_sec);
  } else {
    snprintf(buf, sizeof(buf), "/cm?pw=%s&sid=%d&en=0", pw.c_str(), sid);
  }
  return host + buf;
}

std::string build_cv_url(const std::string& host, const std::string& pw) {
  return host + "/cv?pw=" + pw + "&rsn=1";
}

// ---------------------------------------------------------------------------
// Parsers
// ---------------------------------------------------------------------------

bool parse_jn(const std::string& body, JnData& out) {
  JsonDocument doc;
  if (deserializeJson(doc, body) != DeserializationError::Ok) return false;

  JsonArray snames = doc["snames"];
  if (!snames) return false;

  out.snames.clear();
  for (JsonVariant v : snames) {
    out.snames.push_back(v.as<std::string>());
  }

  auto load_bytes = [&](const char* key, std::vector<uint8_t>& vec) {
    vec.clear();
    JsonArray arr = doc[key];
    if (arr) {
      for (JsonVariant v : arr) vec.push_back(static_cast<uint8_t>(v.as<int>()));
    }
  };

  load_bytes("stn_dis", out.stn_dis);
  load_bytes("masop", out.masop);
  load_bytes("masop2", out.masop2);

  return true;
}

bool parse_jc(const std::string& body, JcData& out) {
  JsonDocument doc;
  if (deserializeJson(doc, body) != DeserializationError::Ok) return false;

  if (!doc["devt"].is<int>()) return false;
  out.devt = doc["devt"].as<int>();
  out.rssi = doc["RSSI"] | 0;

  out.sbits.clear();
  JsonArray sbits = doc["sbits"];
  if (sbits) {
    for (JsonVariant v : sbits)
      out.sbits.push_back(static_cast<uint8_t>(v.as<int>()));
  }

  out.ps.clear();
  JsonArray ps = doc["ps"];
  if (ps) {
    for (JsonVariant entry : ps) {
      PsEntry e;
      if (entry.is<JsonArray>()) {
        JsonArray row = entry.as<JsonArray>();
        e.pid = row[0] | 0;
        e.rem = row[1] | 0;
        e.start = row[2] | 0;
      }
      out.ps.push_back(e);
    }
  }

  return true;
}

OsResult parse_result(const std::string& body) {
  JsonDocument doc;
  if (deserializeJson(doc, body) != DeserializationError::Ok) {
    return OsResult::NetworkError;
  }
  if (!doc["result"].is<int>()) return OsResult::NetworkError;
  const int code = doc["result"].as<int>();
  switch (code) {
    case 1:  return OsResult::Ok;
    case 2:  return OsResult::Unauthorized;
    case 32: return OsResult::NotPermitted;
    default: return OsResult::NetworkError;
  }
}

// ---------------------------------------------------------------------------
// OsClient
// ---------------------------------------------------------------------------

OsClient::OsClient(const std::string& host, const std::string& pw,
                   Transport xport)
    : host_(host), pw_hex_(md5_hex(pw)), transport_(std::move(xport)) {}

bool OsClient::fetch_jn(JnData& out) {
  const std::string body = transport_(build_jn_url(host_, pw_hex_));
  if (body.empty()) { connected_ = false; return false; }
  connected_ = parse_jn(body, out);
  return connected_;
}

bool OsClient::fetch_jc(JcData& out) {
  const std::string body = transport_(build_jc_url(host_, pw_hex_));
  if (body.empty()) { connected_ = false; return false; }
  connected_ = parse_jc(body, out);
  return connected_;
}

bool OsClient::run_station(int sid, int t_sec) {
  const std::string url = build_cm_url(host_, pw_hex_, sid, true, t_sec);
  const std::string body = transport_(url);
  if (body.empty()) { connected_ = false; return false; }
  const bool ok = (parse_result(body) == OsResult::Ok);
  connected_ = ok;
  return ok;
}

bool OsClient::stop_station(int sid) {
  const std::string url = build_cm_url(host_, pw_hex_, sid, false);
  const std::string body = transport_(url);
  if (body.empty()) { connected_ = false; return false; }
  const bool ok = (parse_result(body) == OsResult::Ok);
  connected_ = ok;
  return ok;
}

bool OsClient::stop_all() {
  const std::string url = build_cv_url(host_, pw_hex_);
  const std::string body = transport_(url);
  if (body.empty()) { connected_ = false; return false; }
  const bool ok = (parse_result(body) == OsResult::Ok);
  connected_ = ok;
  return ok;
}

bool OsClient::advance(int from_sid, int to_sid, int t_sec) {
  if (!stop_station(from_sid)) return false;
  return run_station(to_sid, t_sec);
}

bool OsClient::extend(int sid, int t_sec) {
  if (!stop_station(sid)) return false;
  return run_station(sid, t_sec);
}

}  // namespace osp
