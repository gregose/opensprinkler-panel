// Hardware-independent OpenSprinkler HTTP API client.
//
// Pure C++ (no Arduino, no network) — the transport is injected so firmware
// passes an HTTPClient lambda while native tests pass a recording lambda.
//
// Contract: docs/02-opensprinkler-api.md. Critical quirk: en=1 on an already-
// running station is a no-op, so advance/extend do off-then-on (two /cm calls).
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace osp {

// ---------------------------------------------------------------------------
// Result codes returned by the controller in {"result":<code>}.
// ---------------------------------------------------------------------------
enum class OsResult {
  Ok = 1,
  Unauthorized = 2,
  NotPermitted = 32,
  NetworkError = -1,  // transport failed / timeout
};

// ---------------------------------------------------------------------------
// Data from GET /jn (station configuration, fetched once at startup).
// ---------------------------------------------------------------------------
struct JnData {
  std::vector<std::string> snames;   // station display names
  std::vector<uint8_t> stn_dis;      // disabled bitmask (per-board bytes)
  std::vector<uint8_t> masop;        // master-1 association mask (which stations
                                     // open master valve 1 — NOT master identity)
  std::vector<uint8_t> masop2;       // master-2 association mask (see masop)
};

// ---------------------------------------------------------------------------
// Data from GET /jo (controller options). The master station indices live here
// (not in /jn or /jc). Convention: 0 = no master, otherwise the master's
// 1-based station number. masop/masop2 in /jn are only *association* masks.
// ---------------------------------------------------------------------------
struct JoData {
  int mas = 0;   // primary master station (1-based; 0 = none)
  int mas2 = 0;  // secondary master station (1-based; 0 = none)
};

// ---------------------------------------------------------------------------
// One entry from /jc's `ps` array: [pid, rem, start, grp].
// ---------------------------------------------------------------------------
struct PsEntry {
  int pid = 0;    // program id; 0 = idle, 99 = manual /cm run
  int rem = 0;    // seconds remaining
  int start = 0;  // scheduled start epoch (0 = not queued)
};

// ---------------------------------------------------------------------------
// Data from GET /jc (controller status, polled ~every 2 s while running).
// ---------------------------------------------------------------------------
struct JcData {
  int devt = 0;                  // controller local epoch time ("now")
  int rssi = 0;                  // controller Wi-Fi RSSI, dBm (0 if absent)
  std::vector<uint8_t> sbits;   // per-board on/off bitmask (trailing 0)
  std::vector<PsEntry> ps;      // one entry per station, in sid order
};

// ---------------------------------------------------------------------------
// URL builders — return the full request string given host+pw (pre-assembled).
// host = "http://<ip-or-hostname>" (no trailing slash), pw = MD5 hex of password.
// ---------------------------------------------------------------------------
std::string build_jn_url(const std::string& host, const std::string& pw);
std::string build_jc_url(const std::string& host, const std::string& pw);
std::string build_jo_url(const std::string& host, const std::string& pw);

// /cm: run (en=1) or stop (en=0) a single station.
// sid is 0-based. t_sec is only used when en=true.
std::string build_cm_url(const std::string& host, const std::string& pw,
                         int sid, bool en, int t_sec = 0);

// /cv?rsn=1 — stop all stations immediately.
std::string build_cv_url(const std::string& host, const std::string& pw);

// ---------------------------------------------------------------------------
// Parsing — returns false on malformed JSON or missing required fields.
// ---------------------------------------------------------------------------
bool parse_jn(const std::string& body, JnData& out);
bool parse_jc(const std::string& body, JcData& out);
bool parse_jo(const std::string& body, JoData& out);

// Extracts the `result` field. Returns OsResult::NetworkError on parse failure.
OsResult parse_result(const std::string& body);

// ---------------------------------------------------------------------------
// Transport type: given a URL returns the response body, or "" on failure.
// ---------------------------------------------------------------------------
using Transport = std::function<std::string(const std::string& url)>;

// ---------------------------------------------------------------------------
// OsClient — thin wrapper that calls the transport and tracks connected state.
// ---------------------------------------------------------------------------
class OsClient {
 public:
  // host    = "http://<ip-or-hostname>" (no trailing slash)
  // pw_md5  = MD5 hex of the device password, as stored in NVS `os_pw_md5`
  //           (hashed once at provisioning via arduino-esp32 MD5Builder — the
  //           pure client never sees or hashes plaintext).
  // xport   = injected transport function
  OsClient(const std::string& host, const std::string& pw_md5, Transport xport);

  // True after the last request succeeded.
  bool connected() const { return connected_; }
  OsResult last_result() const { return last_result_; }

  // Fetch station configuration (/jn).  Returns false on error.
  bool fetch_jn(JnData& out);

  // Fetch controller status (/jc).  Returns false on error.
  bool fetch_jc(JcData& out);

  // Fetch controller options (/jo) — used for the master station indices
  // (mas/mas2), which are not present in /jn or /jc.  Returns false on error.
  bool fetch_jo(JoData& out);

  // Run a station (en=1 /cm).
  bool run_station(int sid, int t_sec);

  // Stop a station (en=0 /cm).
  bool stop_station(int sid);

  // Stop all stations (/cv?rsn=1).
  bool stop_all();

  // Advance: stop from_sid, then run to_sid for t_sec.
  // Enforces the off-then-on sequence required by firmware (en=1 on a running
  // station is a no-op — docs/02 §/cm).
  bool advance(int from_sid, int to_sid, int t_sec);

  // Extend: stop sid, then run it again for t_sec.
  bool extend(int sid, int t_sec);

 private:
  std::string host_;
  std::string pw_hex_;   // MD5 hex of the device password (supplied, from NVS)
  Transport transport_;
  bool connected_ = false;
  OsResult last_result_ = OsResult::NetworkError;
};

}  // namespace osp
