// Native (host) unit tests for the hardware-independent OpenSprinkler API client.
// Runs under `pio test -e native` — no board or network required.
#include <unity.h>

#include <cstring>
#include <string>
#include <vector>

#include "os_client.h"

using namespace osp;

void setUp() {}
void tearDown() {}

// ---------------------------------------------------------------------------
// URL builders
// ---------------------------------------------------------------------------

void test_build_jn_url() {
  const std::string url = build_jn_url("http://192.168.1.100",
                                       "a6d82bced638de3def1e9bbb4983225c");
  TEST_ASSERT_EQUAL_STRING(
      "http://192.168.1.100/jn?pw=a6d82bced638de3def1e9bbb4983225c",
      url.c_str());
}

void test_build_jc_url() {
  const std::string url = build_jc_url("http://192.168.1.100",
                                       "a6d82bced638de3def1e9bbb4983225c");
  TEST_ASSERT_EQUAL_STRING(
      "http://192.168.1.100/jc?pw=a6d82bced638de3def1e9bbb4983225c",
      url.c_str());
}

void test_build_jp_url() {
  const std::string url = build_jp_url("http://192.168.1.100",
                                       "a6d82bced638de3def1e9bbb4983225c");
  TEST_ASSERT_EQUAL_STRING(
      "http://192.168.1.100/jp?pw=a6d82bced638de3def1e9bbb4983225c",
      url.c_str());
}

void test_build_cm_url_run() {
  const std::string url = build_cm_url("http://192.168.1.100",
                                       "a6d82bced638de3def1e9bbb4983225c",
                                       3, true, 120);
  TEST_ASSERT_EQUAL_STRING(
      "http://192.168.1.100/cm?pw=a6d82bced638de3def1e9bbb4983225c&sid=3&en=1&t=120",
      url.c_str());
}

void test_build_cm_url_stop() {
  const std::string url = build_cm_url("http://192.168.1.100",
                                       "a6d82bced638de3def1e9bbb4983225c",
                                       3, false);
  TEST_ASSERT_EQUAL_STRING(
      "http://192.168.1.100/cm?pw=a6d82bced638de3def1e9bbb4983225c&sid=3&en=0",
      url.c_str());
}

void test_build_cv_url() {
  const std::string url = build_cv_url("http://192.168.1.100",
                                       "a6d82bced638de3def1e9bbb4983225c");
  TEST_ASSERT_EQUAL_STRING(
      "http://192.168.1.100/cv?pw=a6d82bced638de3def1e9bbb4983225c&rsn=1",
      url.c_str());
}

void test_build_jo_url() {
  const std::string url = build_jo_url("http://192.168.1.100",
                                       "a6d82bced638de3def1e9bbb4983225c");
  TEST_ASSERT_EQUAL_STRING(
      "http://192.168.1.100/jo?pw=a6d82bced638de3def1e9bbb4983225c",
      url.c_str());
}

void test_build_mp_url() {
  const std::string url = build_mp_url("http://192.168.1.100",
                                       "a6d82bced638de3def1e9bbb4983225c", 3);
  TEST_ASSERT_EQUAL_STRING(
      "http://192.168.1.100/mp?pw=a6d82bced638de3def1e9bbb4983225c&pid=3&uwt=0&qo=2",
      url.c_str());
}

void test_build_cp_url_enable() {
  const std::string url = build_cp_url("http://192.168.1.100",
                                       "a6d82bced638de3def1e9bbb4983225c",
                                       2, true);
  TEST_ASSERT_EQUAL_STRING(
      "http://192.168.1.100/cp?pw=a6d82bced638de3def1e9bbb4983225c&pid=2&en=1",
      url.c_str());
}

void test_build_cp_url_disable() {
  const std::string url = build_cp_url("http://192.168.1.100",
                                       "a6d82bced638de3def1e9bbb4983225c",
                                       2, false);
  TEST_ASSERT_EQUAL_STRING(
      "http://192.168.1.100/cp?pw=a6d82bced638de3def1e9bbb4983225c&pid=2&en=0",
      url.c_str());
}

void test_build_pq_url() {
  const std::string url = build_pq_url("http://192.168.1.100",
                                       "a6d82bced638de3def1e9bbb4983225c",
                                       600);
  TEST_ASSERT_EQUAL_STRING(
      "http://192.168.1.100/pq?pw=a6d82bced638de3def1e9bbb4983225c&dur=600",
      url.c_str());
}

// ---------------------------------------------------------------------------
// parse_jn
// ---------------------------------------------------------------------------

static const char* kJnBody = R"({
  "snames": ["Front Lawn", "Driveway", "North Beds"],
  "stn_dis": [4],
  "masop":   [32],
  "masop2":  [0]
})";

void test_parse_jn_fields() {
  JnData d;
  TEST_ASSERT_TRUE(parse_jn(kJnBody, d));
  TEST_ASSERT_EQUAL_INT(3, static_cast<int>(d.snames.size()));
  TEST_ASSERT_EQUAL_STRING("Front Lawn", d.snames[0].c_str());
  TEST_ASSERT_EQUAL_STRING("Driveway", d.snames[1].c_str());
  TEST_ASSERT_EQUAL_STRING("North Beds", d.snames[2].c_str());
  TEST_ASSERT_EQUAL_INT(1, static_cast<int>(d.stn_dis.size()));
  TEST_ASSERT_EQUAL_INT(4, d.stn_dis[0]);    // sid 2 disabled
  TEST_ASSERT_EQUAL_INT(32, d.masop[0]);     // sid 5 pump-associated (masop bit)
  TEST_ASSERT_EQUAL_INT(0, d.masop2[0]);
}

void test_parse_jn_malformed() {
  JnData d;
  TEST_ASSERT_FALSE(parse_jn("not json", d));
  TEST_ASSERT_FALSE(parse_jn("{}", d));          // missing snames
  TEST_ASSERT_FALSE(parse_jn("{\"x\":1}", d));
}

// ---------------------------------------------------------------------------
// parse_jc
// ---------------------------------------------------------------------------

static const char* kJcBody = R"({
  "devt": 1719720000,
  "RSSI": -68,
  "sunrise": 362,
  "sunset": 1201,
  "pq": 1,
  "pt": 420,
  "sbits": [2, 0],
  "ps": [
    [0, 0, 0, 0],
    [99, 300, 1719719700, 7],
    [0, 0, 0, 0]
  ]
})";

void test_parse_jc_fields() {
  JcData d;
  TEST_ASSERT_TRUE(parse_jc(kJcBody, d));
  TEST_ASSERT_EQUAL_INT(1719720000, d.devt);
  TEST_ASSERT_EQUAL_INT(-68, d.rssi);
  TEST_ASSERT_EQUAL_INT(362, d.sunrise);
  TEST_ASSERT_EQUAL_INT(1201, d.sunset);
  TEST_ASSERT_EQUAL_INT(1, d.pq);
  TEST_ASSERT_EQUAL_INT(420, d.pt);
  TEST_ASSERT_EQUAL_INT(2, static_cast<int>(d.sbits.size()));
  TEST_ASSERT_EQUAL_INT(2, d.sbits[0]);   // station 1 (bit 1) is on
  TEST_ASSERT_EQUAL_INT(0, d.sbits[1]);
  TEST_ASSERT_EQUAL_INT(3, static_cast<int>(d.ps.size()));
  TEST_ASSERT_EQUAL_INT(0, d.ps[0].pid);
  TEST_ASSERT_EQUAL_INT(99, d.ps[1].pid);
  TEST_ASSERT_EQUAL_INT(300, d.ps[1].rem);
  TEST_ASSERT_EQUAL_INT(1719719700, d.ps[1].start);
  TEST_ASSERT_EQUAL_INT(7, d.ps[1].gid);
}

void test_parse_jc_missing_rssi() {
  // RSSI is optional (not present on some firmware versions).
  JcData d;
  TEST_ASSERT_TRUE(parse_jc(R"({"devt":1000,"sbits":[0],"ps":[[99,10,20]]})", d));
  TEST_ASSERT_EQUAL_INT(0, d.rssi);
  TEST_ASSERT_EQUAL_INT(0, d.sunrise);
  TEST_ASSERT_EQUAL_INT(0, d.sunset);
  TEST_ASSERT_EQUAL_INT(0, d.pq);
  TEST_ASSERT_EQUAL_INT(0, d.pt);
  TEST_ASSERT_EQUAL_INT(0, d.ps[0].gid);
}

void test_parse_jc_malformed() {
  JcData d;
  TEST_ASSERT_FALSE(parse_jc("not json", d));
  TEST_ASSERT_FALSE(parse_jc("{}", d));    // missing devt
}

// ---------------------------------------------------------------------------
// parse_jo (master station indices)
// ---------------------------------------------------------------------------

void test_parse_jo_fields() {
  JoData d;
  TEST_ASSERT_TRUE(parse_jo(R"({"mas":6,"mas2":10})", d));
  TEST_ASSERT_EQUAL_INT(6, d.mas);
  TEST_ASSERT_EQUAL_INT(10, d.mas2);
}

void test_parse_jo_none_and_missing() {
  // Greg's controller: mas=0 => no master; mas2 absent => defaults to 0.
  JoData d;
  TEST_ASSERT_TRUE(parse_jo(R"({"mas":0})", d));
  TEST_ASSERT_EQUAL_INT(0, d.mas);
  TEST_ASSERT_EQUAL_INT(0, d.mas2);
}

void test_parse_jo_malformed() {
  JoData d;
  TEST_ASSERT_FALSE(parse_jo("not json", d));
}

// ---------------------------------------------------------------------------
// parse_jp
// ---------------------------------------------------------------------------

static const char* kJpBody = R"({
  "nprogs": 2,
  "nboards": 1,
  "mnp": 40,
  "mnst": 8,
  "pnsize": 32,
  "pd": [
    [65, 127, 0, [360, 0, 0, 0], [0, 120, 30], "Weekly", [0, 0, 0]],
    [187, 5, 7, [60, 120], [300, 0, 10], "Interval", [1, 42, 88]]
  ]
})";

void test_parse_jp_fields() {
  JpData d;
  TEST_ASSERT_TRUE(parse_jp(kJpBody, d));
  TEST_ASSERT_EQUAL_INT(2, d.nprogs);
  TEST_ASSERT_EQUAL_INT(1, d.nboards);
  TEST_ASSERT_EQUAL_INT(40, d.mnp);
  TEST_ASSERT_EQUAL_INT(8, d.mnst);
  TEST_ASSERT_EQUAL_INT(32, d.pnsize);
  TEST_ASSERT_EQUAL_INT(2, static_cast<int>(d.programs.size()));

  const Program& weekly = d.programs[0];
  TEST_ASSERT_TRUE(weekly.enabled);
  TEST_ASSERT_FALSE(weekly.use_weather);
  TEST_ASSERT_EQUAL_INT((int)ProgramType::Weekly, (int)weekly.type);
  TEST_ASSERT_TRUE(weekly.starttime_type_fixed);
  TEST_ASSERT_EQUAL_INT(2, weekly.station_count());
  TEST_ASSERT_EQUAL_INT(150, weekly.total_seconds());
  TEST_ASSERT_EQUAL_STRING("Weekly", weekly.name.c_str());

  const Program& interval = d.programs[1];
  TEST_ASSERT_TRUE(interval.enabled);
  TEST_ASSERT_TRUE(interval.use_weather);
  TEST_ASSERT_EQUAL_INT((int)OddEven::Even, (int)interval.oddeven);
  TEST_ASSERT_EQUAL_INT((int)ProgramType::Interval, (int)interval.type);
  TEST_ASSERT_FALSE(interval.starttime_type_fixed);
  TEST_ASSERT_TRUE(interval.en_daterange);
  TEST_ASSERT_EQUAL_INT(2, interval.station_count());
  TEST_ASSERT_EQUAL_INT(310, interval.total_seconds());
  TEST_ASSERT_EQUAL_INT(0, interval.starttimes[2]);
  TEST_ASSERT_EQUAL_INT(42, interval.daterange[1]);
  TEST_ASSERT_EQUAL_INT(88, interval.daterange[2]);
}

void test_parse_jp_malformed() {
  JpData d;
  TEST_ASSERT_FALSE(parse_jp("not json", d));
  TEST_ASSERT_FALSE(parse_jp("{}", d));
  TEST_ASSERT_FALSE(parse_jp(R"({"pd":{}})", d));
}

// ---------------------------------------------------------------------------
// parse_result
// ---------------------------------------------------------------------------

void test_parse_result_ok() {
  TEST_ASSERT_EQUAL_INT(static_cast<int>(OsResult::Ok),
                        static_cast<int>(parse_result(R"({"result":1})")));
}

void test_parse_result_unauthorized() {
  TEST_ASSERT_EQUAL_INT(static_cast<int>(OsResult::Unauthorized),
                        static_cast<int>(parse_result(R"({"result":2})")));
}

void test_parse_result_not_permitted() {
  TEST_ASSERT_EQUAL_INT(static_cast<int>(OsResult::NotPermitted),
                        static_cast<int>(parse_result(R"({"result":32})")));
}

void test_parse_result_network_error() {
  TEST_ASSERT_EQUAL_INT(static_cast<int>(OsResult::NetworkError),
                        static_cast<int>(parse_result("bad")));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(OsResult::NetworkError),
                        static_cast<int>(parse_result("{}")));
}

// ---------------------------------------------------------------------------
// OsClient — recording transport
// ---------------------------------------------------------------------------

// Helper: a transport that returns a fixed response and records all URLs called.
struct RecordingTransport {
  std::string response;
  std::vector<std::string> calls;

  std::string operator()(const std::string& url) {
    calls.push_back(url);
    return response;
  }
};

static const std::string kHost = "http://192.168.1.100";
// The client is constructed with the pre-hashed pw (as stored in NVS
// `os_pw_md5`); this is md5("opendoor"), the default OS password.
static const std::string kMd5 = "a6d82bced638de3def1e9bbb4983225c";

void test_client_fetch_jn() {
  RecordingTransport rt;
  rt.response = kJnBody;
  OsClient c(kHost, kMd5, [&rt](const std::string& url) { return rt(url); });

  JnData d;
  TEST_ASSERT_TRUE(c.fetch_jn(d));
  TEST_ASSERT_TRUE(c.connected());
  TEST_ASSERT_EQUAL_INT(1, static_cast<int>(rt.calls.size()));
  TEST_ASSERT_EQUAL_STRING(
      ("http://192.168.1.100/jn?pw=" + kMd5).c_str(),
      rt.calls[0].c_str());
  TEST_ASSERT_EQUAL_INT(3, static_cast<int>(d.snames.size()));
}

void test_client_fetch_jc() {
  RecordingTransport rt;
  rt.response = kJcBody;
  OsClient c(kHost, kMd5, [&rt](const std::string& url) { return rt(url); });

  JcData d;
  TEST_ASSERT_TRUE(c.fetch_jc(d));
  TEST_ASSERT_TRUE(c.connected());
  TEST_ASSERT_EQUAL_INT(1, static_cast<int>(rt.calls.size()));
  TEST_ASSERT_EQUAL_STRING(
      ("http://192.168.1.100/jc?pw=" + kMd5).c_str(),
      rt.calls[0].c_str());
}

void test_client_fetch_jo() {
  RecordingTransport rt;
  rt.response = R"({"mas":0,"mas2":0})";
  OsClient c(kHost, kMd5, [&rt](const std::string& url) { return rt(url); });

  JoData d;
  TEST_ASSERT_TRUE(c.fetch_jo(d));
  TEST_ASSERT_TRUE(c.connected());
  TEST_ASSERT_EQUAL_INT(1, static_cast<int>(rt.calls.size()));
  TEST_ASSERT_EQUAL_STRING(
      ("http://192.168.1.100/jo?pw=" + kMd5).c_str(),
      rt.calls[0].c_str());
  TEST_ASSERT_EQUAL_INT(0, d.mas);
  TEST_ASSERT_EQUAL_INT(0, d.mas2);
}

void test_client_fetch_jp() {
  RecordingTransport rt;
  rt.response = kJpBody;
  OsClient c(kHost, kMd5, [&rt](const std::string& url) { return rt(url); });

  JpData d;
  TEST_ASSERT_TRUE(c.fetch_jp(d));
  TEST_ASSERT_TRUE(c.connected());
  TEST_ASSERT_EQUAL_INT(static_cast<int>(OsResult::Ok),
                        static_cast<int>(c.last_result()));
  TEST_ASSERT_EQUAL_INT(1, static_cast<int>(rt.calls.size()));
  TEST_ASSERT_EQUAL_STRING(
      ("http://192.168.1.100/jp?pw=" + kMd5).c_str(),
      rt.calls[0].c_str());
  TEST_ASSERT_EQUAL_INT(2, static_cast<int>(d.programs.size()));
}

void test_client_run_station() {
  RecordingTransport rt;
  rt.response = R"({"result":1})";
  OsClient c(kHost, kMd5, [&rt](const std::string& url) { return rt(url); });

  TEST_ASSERT_TRUE(c.run_station(2, 180));
  TEST_ASSERT_TRUE(c.connected());
  TEST_ASSERT_EQUAL_INT(1, static_cast<int>(rt.calls.size()));
  // URL must have sid=2, en=1, t=180
  const std::string& url = rt.calls[0];
  TEST_ASSERT_NOT_NULL(strstr(url.c_str(), "sid=2"));
  TEST_ASSERT_NOT_NULL(strstr(url.c_str(), "en=1"));
  TEST_ASSERT_NOT_NULL(strstr(url.c_str(), "t=180"));
}

void test_client_stop_station() {
  RecordingTransport rt;
  rt.response = R"({"result":1})";
  OsClient c(kHost, kMd5, [&rt](const std::string& url) { return rt(url); });

  TEST_ASSERT_TRUE(c.stop_station(4));
  TEST_ASSERT_EQUAL_INT(1, static_cast<int>(rt.calls.size()));
  const std::string& url = rt.calls[0];
  TEST_ASSERT_NOT_NULL(strstr(url.c_str(), "sid=4"));
  TEST_ASSERT_NOT_NULL(strstr(url.c_str(), "en=0"));
  // en=0 must NOT have &t= in the URL
  TEST_ASSERT_NULL(strstr(url.c_str(), "&t="));
}

void test_client_stop_all() {
  RecordingTransport rt;
  rt.response = R"({"result":1})";
  OsClient c(kHost, kMd5, [&rt](const std::string& url) { return rt(url); });

  TEST_ASSERT_TRUE(c.stop_all());
  TEST_ASSERT_EQUAL_INT(1, static_cast<int>(rt.calls.size()));
  TEST_ASSERT_NOT_NULL(strstr(rt.calls[0].c_str(), "/cv?"));
  TEST_ASSERT_NOT_NULL(strstr(rt.calls[0].c_str(), "rsn=1"));
}

void test_client_run_program() {
  RecordingTransport rt;
  rt.response = R"({"result":1})";
  OsClient c(kHost, kMd5, [&rt](const std::string& url) { return rt(url); });

  TEST_ASSERT_TRUE(c.run_program(3));
  TEST_ASSERT_TRUE(c.connected());
  TEST_ASSERT_EQUAL_INT(static_cast<int>(OsResult::Ok),
                        static_cast<int>(c.last_result()));
  TEST_ASSERT_EQUAL_INT(1, static_cast<int>(rt.calls.size()));
  TEST_ASSERT_EQUAL_STRING(build_mp_url(kHost, kMd5, 3).c_str(),
                           rt.calls[0].c_str());
}

void test_client_run_program_unauthorized() {
  RecordingTransport rt;
  rt.response = R"({"result":2})";
  OsClient c(kHost, kMd5, [&rt](const std::string& url) { return rt(url); });

  TEST_ASSERT_FALSE(c.run_program(1));
  TEST_ASSERT_FALSE(c.connected());
  TEST_ASSERT_EQUAL_INT(static_cast<int>(OsResult::Unauthorized),
                        static_cast<int>(c.last_result()));
}

void test_client_set_program_enabled_not_permitted() {
  RecordingTransport rt;
  rt.response = R"({"result":32})";
  OsClient c(kHost, kMd5, [&rt](const std::string& url) { return rt(url); });

  TEST_ASSERT_FALSE(c.set_program_enabled(2, false));
  TEST_ASSERT_FALSE(c.connected());
  TEST_ASSERT_EQUAL_INT(static_cast<int>(OsResult::NotPermitted),
                        static_cast<int>(c.last_result()));
  TEST_ASSERT_EQUAL_INT(1, static_cast<int>(rt.calls.size()));
  TEST_ASSERT_EQUAL_STRING(build_cp_url(kHost, kMd5, 2, false).c_str(),
                           rt.calls[0].c_str());
}

void test_client_pause_default_duration() {
  RecordingTransport rt;
  rt.response = R"({"result":1})";
  OsClient c(kHost, kMd5, [&rt](const std::string& url) { return rt(url); });

  TEST_ASSERT_TRUE(c.pause());
  TEST_ASSERT_TRUE(c.connected());
  TEST_ASSERT_EQUAL_INT(static_cast<int>(OsResult::Ok),
                        static_cast<int>(c.last_result()));
  TEST_ASSERT_EQUAL_INT(1, static_cast<int>(rt.calls.size()));
  TEST_ASSERT_EQUAL_STRING(build_pq_url(kHost, kMd5, 600).c_str(),
                           rt.calls[0].c_str());
}

void test_client_advance_off_then_on() {
  // advance must issue stop(from) then run(to) in that exact order.
  RecordingTransport rt;
  rt.response = R"({"result":1})";
  OsClient c(kHost, kMd5, [&rt](const std::string& url) { return rt(url); });

  TEST_ASSERT_TRUE(c.advance(1, 3, 240));
  TEST_ASSERT_EQUAL_INT(2, static_cast<int>(rt.calls.size()));

  // First call: stop sid=1 (en=0)
  TEST_ASSERT_NOT_NULL(strstr(rt.calls[0].c_str(), "sid=1"));
  TEST_ASSERT_NOT_NULL(strstr(rt.calls[0].c_str(), "en=0"));

  // Second call: run sid=3 (en=1, t=240)
  TEST_ASSERT_NOT_NULL(strstr(rt.calls[1].c_str(), "sid=3"));
  TEST_ASSERT_NOT_NULL(strstr(rt.calls[1].c_str(), "en=1"));
  TEST_ASSERT_NOT_NULL(strstr(rt.calls[1].c_str(), "t=240"));
}

void test_client_advance_aborts_if_stop_fails() {
  // If the first (stop) call fails, advance must not issue the run call.
  RecordingTransport rt;
  rt.response = "";  // empty = network error
  OsClient c(kHost, kMd5, [&rt](const std::string& url) { return rt(url); });

  TEST_ASSERT_FALSE(c.advance(1, 3, 240));
  TEST_ASSERT_EQUAL_INT(1, static_cast<int>(rt.calls.size()));  // only the stop
  TEST_ASSERT_FALSE(c.connected());
}

void test_client_extend_off_then_on_same_sid() {
  RecordingTransport rt;
  rt.response = R"({"result":1})";
  OsClient c(kHost, kMd5, [&rt](const std::string& url) { return rt(url); });

  TEST_ASSERT_TRUE(c.extend(2, 300));
  TEST_ASSERT_EQUAL_INT(2, static_cast<int>(rt.calls.size()));

  // Both calls reference the same sid=2.
  TEST_ASSERT_NOT_NULL(strstr(rt.calls[0].c_str(), "sid=2"));
  TEST_ASSERT_NOT_NULL(strstr(rt.calls[0].c_str(), "en=0"));

  TEST_ASSERT_NOT_NULL(strstr(rt.calls[1].c_str(), "sid=2"));
  TEST_ASSERT_NOT_NULL(strstr(rt.calls[1].c_str(), "en=1"));
  TEST_ASSERT_NOT_NULL(strstr(rt.calls[1].c_str(), "t=300"));
}

void test_client_network_error_sets_disconnected() {
  RecordingTransport rt;
  rt.response = "";  // simulate timeout/failure
  OsClient c(kHost, kMd5, [&rt](const std::string& url) { return rt(url); });

  JcData d;
  TEST_ASSERT_FALSE(c.fetch_jc(d));
  TEST_ASSERT_FALSE(c.connected());
}

void test_client_pause_network_error_sets_disconnected() {
  RecordingTransport rt;
  rt.response = "";
  OsClient c(kHost, kMd5, [&rt](const std::string& url) { return rt(url); });

  TEST_ASSERT_FALSE(c.pause());
  TEST_ASSERT_FALSE(c.connected());
  TEST_ASSERT_EQUAL_INT(static_cast<int>(OsResult::NetworkError),
                        static_cast<int>(c.last_result()));
}

void test_to_program_ps_round_trips_into_run_state() {
  JcData jc;
  jc.ps = {
      PsEntry{2, 40, 900, 11},
      PsEntry{2, 30, 1100, 12},
      PsEntry{2, 20, 800, 13},
      PsEntry{0, 0, 0, 0},
  };

  std::vector<ProgramPsEntry> ps = to_program_ps(jc);
  TEST_ASSERT_EQUAL_INT(4, static_cast<int>(ps.size()));
  TEST_ASSERT_EQUAL_INT(2, ps[0].pid);
  TEST_ASSERT_EQUAL_INT(40, ps[0].rem);
  TEST_ASSERT_EQUAL_INT(900, ps[0].start);
  TEST_ASSERT_EQUAL_INT(12, ps[1].gid);

  ProgramRunState state = resolve_program_run_state(ps, 4, 1000);
  TEST_ASSERT_EQUAL_INT((int)RunClass::ProgramRun, (int)state.run_class);
  TEST_ASSERT_EQUAL_INT(1, state.program_index);
  TEST_ASSERT_EQUAL_INT(0, state.current_sid);
  TEST_ASSERT_EQUAL_INT(2, state.current_station_number);
  TEST_ASSERT_EQUAL_INT(3, state.station_count);
}

// ---------------------------------------------------------------------------

int main(int, char**) {
  UNITY_BEGIN();


  RUN_TEST(test_build_jn_url);
  RUN_TEST(test_build_jc_url);
  RUN_TEST(test_build_jp_url);
  RUN_TEST(test_build_cm_url_run);
  RUN_TEST(test_build_cm_url_stop);
  RUN_TEST(test_build_cv_url);
  RUN_TEST(test_build_jo_url);
  RUN_TEST(test_build_mp_url);
  RUN_TEST(test_build_cp_url_enable);
  RUN_TEST(test_build_cp_url_disable);
  RUN_TEST(test_build_pq_url);

  RUN_TEST(test_parse_jn_fields);
  RUN_TEST(test_parse_jn_malformed);

  RUN_TEST(test_parse_jc_fields);
  RUN_TEST(test_parse_jc_missing_rssi);
  RUN_TEST(test_parse_jc_malformed);

  RUN_TEST(test_parse_jo_fields);
  RUN_TEST(test_parse_jo_none_and_missing);
  RUN_TEST(test_parse_jo_malformed);

  RUN_TEST(test_parse_jp_fields);
  RUN_TEST(test_parse_jp_malformed);

  RUN_TEST(test_parse_result_ok);
  RUN_TEST(test_parse_result_unauthorized);
  RUN_TEST(test_parse_result_not_permitted);
  RUN_TEST(test_parse_result_network_error);

  RUN_TEST(test_client_fetch_jn);
  RUN_TEST(test_client_fetch_jc);
  RUN_TEST(test_client_fetch_jo);
  RUN_TEST(test_client_fetch_jp);
  RUN_TEST(test_client_run_station);
  RUN_TEST(test_client_stop_station);
  RUN_TEST(test_client_stop_all);
  RUN_TEST(test_client_run_program);
  RUN_TEST(test_client_run_program_unauthorized);
  RUN_TEST(test_client_set_program_enabled_not_permitted);
  RUN_TEST(test_client_pause_default_duration);
  RUN_TEST(test_client_advance_off_then_on);
  RUN_TEST(test_client_advance_aborts_if_stop_fails);
  RUN_TEST(test_client_extend_off_then_on_same_sid);
  RUN_TEST(test_client_network_error_sets_disconnected);
  RUN_TEST(test_client_pause_network_error_sets_disconnected);
  RUN_TEST(test_to_program_ps_round_trips_into_run_state);

  return UNITY_END();
}
