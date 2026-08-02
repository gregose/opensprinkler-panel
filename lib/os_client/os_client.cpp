#include "os_client.h"

#include <ArduinoJson.h>

#include <cstdio>
#include <cctype>
#include <limits>

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

std::string build_jo_url(const std::string& host, const std::string& pw) {
  return host + "/jo?pw=" + pw;
}

std::string build_jp_url(const std::string& host, const std::string& pw) {
  return host + "/jp?pw=" + pw;
}

std::string build_jl_url(const std::string& host, const std::string& pw,
                         int history_days) {
  char buf[256];
  snprintf(buf, sizeof(buf), "/jl?pw=%s&hist=%d", pw.c_str(), history_days);
  return host + buf;
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

std::string build_mp_url(const std::string& host, const std::string& pw,
                         int pid) {
  char buf[256];
  snprintf(buf, sizeof(buf), "/mp?pw=%s&pid=%d&uwt=0&qo=2", pw.c_str(), pid);
  return host + buf;
}

std::string build_cp_url(const std::string& host, const std::string& pw,
                         int pid, bool en) {
  char buf[256];
  snprintf(buf, sizeof(buf), "/cp?pw=%s&pid=%d&en=%d",
           pw.c_str(), pid, en ? 1 : 0);
  return host + buf;
}

std::string build_pq_url(const std::string& host, const std::string& pw,
                         int dur) {
  char buf[256];
  snprintf(buf, sizeof(buf), "/pq?pw=%s&dur=%d", pw.c_str(), dur);
  return host + buf;
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
  out.sunrise = doc["sunrise"] | 0;
  out.sunset = doc["sunset"] | 0;
  out.en = doc["en"] | 1;
  out.rd = doc["rd"] | 0;
  out.rdst = doc["rdst"] | 0;
  out.has_curr = doc["curr"].is<int>();
  out.curr = out.has_curr ? doc["curr"].as<int>() : 0;
  out.pq = doc["pq"] | 0;
  out.pt = doc["pt"] | 0;

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
        e.gid = row[3] | 0;
      }
      out.ps.push_back(e);
    }
  }

  return true;
}

bool parse_jp(const std::string& body, JpData& out) {
  JsonDocument doc;
  if (deserializeJson(doc, body) != DeserializationError::Ok) return false;

  JsonArray pd = doc["pd"];
  if (!pd) return false;

  out.nprogs = doc["nprogs"] | 0;
  out.nboards = doc["nboards"] | 0;
  out.mnp = doc["mnp"] | 0;
  out.mnst = doc["mnst"] | 0;
  out.pnsize = doc["pnsize"] | 0;
  out.programs.clear();

  for (JsonVariant entry : pd) {
    if (!entry.is<JsonArray>()) return false;

    JsonArray row = entry.as<JsonArray>();
    std::array<int16_t, 4> starttimes = {0, 0, 0, 0};
    JsonArray starts = row[3];
    if (starts) {
      int i = 0;
      for (JsonVariant v : starts) {
        if (i >= static_cast<int>(starttimes.size())) break;
        starttimes[i++] = static_cast<int16_t>(v.as<int>());
      }
    }

    std::vector<int> durations;
    JsonArray durs = row[4];
    if (durs) {
      for (JsonVariant v : durs) durations.push_back(v.as<int>());
    }

    std::array<int, 3> daterange = {0, 0, 0};
    JsonArray range = row[6];
    if (range) {
      int i = 0;
      for (JsonVariant v : range) {
        if (i >= static_cast<int>(daterange.size())) break;
        daterange[i++] = v.as<int>();
      }
    }

    const char* name = row[5] | "";
    out.programs.push_back(load_program(row[0] | 0,
                                        row[1] | 0,
                                        row[2] | 0,
                                        starttimes,
                                        durations,
                                        name,
                                        daterange));
  }

  return true;
}

bool parse_jo(const std::string& body, JoData& out) {
  JsonDocument doc;
  if (deserializeJson(doc, body) != DeserializationError::Ok) return false;
  if (!doc["mas"].is<int>()) return false;
  // /jo carries the master station options. Missing keys => no master (0).
  out.mas = doc["mas"].as<int>();
  out.mas2 = doc["mas2"] | 0;
  out.dname = doc["dname"] | "";
  return true;
}

namespace {

bool parse_log_row(const char* data, std::size_t length, LogEntry& entry) {
    JsonDocument doc;
    if (deserializeJson(doc, data, length) != DeserializationError::Ok ||
        !doc.is<JsonArray>()) {
      return false;
    }
    JsonArray row = doc.as<JsonArray>();
    if (row.size() != 4 && row.size() != 5) return false;
    if (!row[0].is<int>() || !row[2].is<int64_t>() ||
        !row[3].is<int64_t>()) {
      return false;
    }

    const int pid = row[0].as<int>();
    const int64_t duration = row[2].as<int64_t>();
    const int64_t end_epoch = row[3].as<int64_t>();
    if (pid < 0 || duration < 0 || end_epoch < 0 ||
        duration > std::numeric_limits<uint32_t>::max() ||
        end_epoch > std::numeric_limits<uint32_t>::max()) {
      return false;
    }

    entry.pid = pid;
    entry.duration_s = static_cast<uint32_t>(duration);
    entry.end_epoch = static_cast<uint32_t>(end_epoch);
    if (pid == 0) {
      if (!row[1].is<const char*>()) return false;
      entry.event_code = row[1].as<const char*>();
      if (entry.event_code.empty()) return false;
    } else {
      if (!row[1].is<int>()) return false;
      entry.sid = row[1].as<int>();
      if (entry.sid < 0) return false;
    }

    if (row.size() == 5) {
      if (!row[4].is<float>()) return false;
      entry.has_flow = true;
      entry.flow = row[4].as<float>();
    }
    return true;
}

}  // namespace

JlParser::JlParser(std::vector<LogEntry>& out, std::size_t max_entries,
                   LogEntryFilter filter)
    : out_(out), max_entries_(max_entries), filter_(std::move(filter)) {
  out_.clear();
  item_.reserve(kMaxRowBytes);
}

void JlParser::append_item_char(char c) {
  if (item_.size() < kMaxRowBytes) {
    item_.push_back(c);
  } else {
    item_too_long_ = true;
  }
}

void JlParser::finish_item() {
  if (!item_too_long_) {
    LogEntry entry;
    if (parse_log_row(item_.data(), item_.size(), entry) &&
        (!filter_ || filter_(entry))) {
      if (max_entries_ > 0 && out_.size() == max_entries_) {
        out_.erase(out_.begin());
      }
      out_.push_back(std::move(entry));
    }
  }
  item_.clear();
  item_too_long_ = false;
}

bool JlParser::feed(const char* data, std::size_t length) {
  if (invalid_) return false;

  for (std::size_t i = 0; i < length; ++i) {
    const char c = data[i];
    if (finished_) {
      if (!std::isspace(static_cast<unsigned char>(c))) {
        invalid_ = true;
        return false;
      }
      continue;
    }

    if (!started_) {
      if (std::isspace(static_cast<unsigned char>(c))) continue;
      if (c != '[') {
        invalid_ = true;
        return false;
      }
      started_ = true;
      continue;
    }

    if (!reading_item_) {
      if (std::isspace(static_cast<unsigned char>(c))) continue;
      if (c == ']') {
        if (value_required_) {
          invalid_ = true;
          return false;
        }
        finished_ = true;
        continue;
      }
      if (c == ',') {
        invalid_ = true;
        return false;
      }
      reading_item_ = true;
      value_required_ = false;
      depth_ = 0;
      in_string_ = false;
      escaped_ = false;
    }

    if (in_string_) {
      append_item_char(c);
      if (escaped_) {
        escaped_ = false;
      } else if (c == '\\') {
        escaped_ = true;
      } else if (c == '"') {
        in_string_ = false;
      }
      continue;
    }

    if (c == '"') {
      in_string_ = true;
      append_item_char(c);
    } else if (c == '[' || c == '{') {
      ++depth_;
      append_item_char(c);
    } else if (c == ']' || c == '}') {
      if (c == ']' && depth_ == 0) {
        finish_item();
        reading_item_ = false;
        finished_ = true;
      } else if (depth_ == 0) {
        invalid_ = true;
        return false;
      } else {
        --depth_;
        append_item_char(c);
      }
    } else if (c == ',' && depth_ == 0) {
      finish_item();
      reading_item_ = false;
      value_required_ = true;
    } else {
      append_item_char(c);
    }
  }
  return !invalid_;
}

bool JlParser::finish() const {
  return started_ && finished_ && !invalid_ && !reading_item_;
}

bool parse_jl(const std::string& body, std::vector<LogEntry>& out,
              std::size_t max_entries) {
  JlParser parser(out, max_entries);
  return parser.feed(body.data(), body.size()) && parser.finish();
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

std::vector<ProgramPsEntry> to_program_ps(const JcData& jc) {
  std::vector<ProgramPsEntry> out;
  out.reserve(jc.ps.size());
  for (const PsEntry& e : jc.ps) {
    out.push_back(
        ProgramPsEntry{e.pid, e.rem, static_cast<long>(e.start), e.gid});
  }
  return out;
}

// ---------------------------------------------------------------------------
// OsClient
// ---------------------------------------------------------------------------

OsClient::OsClient(const std::string& host, const std::string& pw_md5,
                   Transport xport, LogTransport log_xport)
    : host_(host),
      pw_hex_(pw_md5),
      transport_(std::move(xport)),
      log_transport_(std::move(log_xport)) {}

bool OsClient::fetch_jn(JnData& out) {
  const std::string body = transport_(build_jn_url(host_, pw_hex_));
  if (body.empty()) {
    connected_ = false;
    last_result_ = OsResult::NetworkError;
    return false;
  }
  if (parse_jn(body, out)) {
    connected_ = true;
    last_result_ = OsResult::Ok;
    return true;
  }
  last_result_ = parse_result(body);
  connected_ = false;
  return false;
}

bool OsClient::fetch_jc(JcData& out) {
  const std::string body = transport_(build_jc_url(host_, pw_hex_));
  if (body.empty()) {
    connected_ = false;
    last_result_ = OsResult::NetworkError;
    return false;
  }
  if (parse_jc(body, out)) {
    connected_ = true;
    last_result_ = OsResult::Ok;
    return true;
  }
  last_result_ = parse_result(body);
  connected_ = false;
  return false;
}

bool OsClient::fetch_jo(JoData& out) {
  const std::string body = transport_(build_jo_url(host_, pw_hex_));
  if (body.empty()) {
    connected_ = false;
    last_result_ = OsResult::NetworkError;
    return false;
  }
  if (parse_jo(body, out)) {
    connected_ = true;
    last_result_ = OsResult::Ok;
    return true;
  }
  last_result_ = parse_result(body);
  connected_ = false;
  return false;
}

bool OsClient::fetch_jp(JpData& out) {
  const std::string body = transport_(build_jp_url(host_, pw_hex_));
  if (body.empty()) {
    connected_ = false;
    last_result_ = OsResult::NetworkError;
    return false;
  }
  if (parse_jp(body, out)) {
    connected_ = true;
    last_result_ = OsResult::Ok;
    return true;
  }
  last_result_ = parse_result(body);
  connected_ = false;
  return false;
}

bool OsClient::fetch_jl(std::vector<LogEntry>& out, int history_days,
                        std::size_t max_entries) {
  const std::string url = build_jl_url(host_, pw_hex_, history_days);
  if (log_transport_) {
    last_result_ = log_transport_(url, max_entries, out);
    connected_ = last_result_ == OsResult::Ok;
    return connected_;
  }

  const std::string body = transport_(url);
  if (body.empty()) {
    connected_ = false;
    last_result_ = OsResult::NetworkError;
    return false;
  }
  if (parse_jl(body, out, max_entries)) {
    connected_ = true;
    last_result_ = OsResult::Ok;
    return true;
  }
  last_result_ = parse_result(body);
  connected_ = false;
  return false;
}

bool OsClient::run_station(int sid, int t_sec) {
  const std::string url = build_cm_url(host_, pw_hex_, sid, true, t_sec);
  const std::string body = transport_(url);
  if (body.empty()) {
    connected_ = false;
    last_result_ = OsResult::NetworkError;
    return false;
  }
  last_result_ = parse_result(body);
  const bool ok = (last_result_ == OsResult::Ok);
  connected_ = ok;
  return ok;
}

bool OsClient::stop_station(int sid) {
  const std::string url = build_cm_url(host_, pw_hex_, sid, false);
  const std::string body = transport_(url);
  if (body.empty()) {
    connected_ = false;
    last_result_ = OsResult::NetworkError;
    return false;
  }
  last_result_ = parse_result(body);
  const bool ok = (last_result_ == OsResult::Ok);
  connected_ = ok;
  return ok;
}

bool OsClient::run_program(int pid) {
  const std::string url = build_mp_url(host_, pw_hex_, pid);
  const std::string body = transport_(url);
  if (body.empty()) {
    connected_ = false;
    last_result_ = OsResult::NetworkError;
    return false;
  }
  last_result_ = parse_result(body);
  const bool ok = (last_result_ == OsResult::Ok);
  connected_ = ok;
  return ok;
}

bool OsClient::set_program_enabled(int pid, bool en) {
  const std::string url = build_cp_url(host_, pw_hex_, pid, en);
  const std::string body = transport_(url);
  if (body.empty()) {
    connected_ = false;
    last_result_ = OsResult::NetworkError;
    return false;
  }
  last_result_ = parse_result(body);
  const bool ok = (last_result_ == OsResult::Ok);
  connected_ = ok;
  return ok;
}

bool OsClient::pause(int dur_sec) {
  const std::string url = build_pq_url(host_, pw_hex_, dur_sec);
  const std::string body = transport_(url);
  if (body.empty()) {
    connected_ = false;
    last_result_ = OsResult::NetworkError;
    return false;
  }
  last_result_ = parse_result(body);
  const bool ok = (last_result_ == OsResult::Ok);
  connected_ = ok;
  return ok;
}

bool OsClient::stop_all() {
  const std::string url = build_cv_url(host_, pw_hex_);
  const std::string body = transport_(url);
  if (body.empty()) {
    connected_ = false;
    last_result_ = OsResult::NetworkError;
    return false;
  }
  last_result_ = parse_result(body);
  const bool ok = (last_result_ == OsResult::Ok);
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

bool OsClient::skip_station(int sid) {
  char buf[256];
  snprintf(buf, sizeof(buf), "/cm?pw=%s&sid=%d&en=0&ssta=1",
           pw_hex_.c_str(), sid);
  const std::string url = host_ + buf;
  const std::string body = transport_(url);
  if (body.empty()) {
    connected_ = false;
    last_result_ = OsResult::NetworkError;
    return false;
  }
  last_result_ = parse_result(body);
  const bool ok = (last_result_ == OsResult::Ok);
  connected_ = ok;
  return ok;
}

}  // namespace osp
