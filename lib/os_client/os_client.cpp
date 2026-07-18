#include "os_client.h"

#include <ArduinoJson.h>

#include <cstdio>

namespace osp {

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

OsClient::OsClient(const std::string& host, const std::string& pw_md5,
                   Transport xport)
    : host_(host), pw_hex_(pw_md5), transport_(std::move(xport)) {}

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
