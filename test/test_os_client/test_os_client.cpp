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
  TEST_ASSERT_EQUAL_INT(32, d.masop[0]);     // sid 5 master
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
  "sbits": [2, 0],
  "ps": [
    [0, 0, 0, 0],
    [99, 300, 1719719700, 0],
    [0, 0, 0, 0]
  ]
})";

void test_parse_jc_fields() {
  JcData d;
  TEST_ASSERT_TRUE(parse_jc(kJcBody, d));
  TEST_ASSERT_EQUAL_INT(1719720000, d.devt);
  TEST_ASSERT_EQUAL_INT(-68, d.rssi);
  TEST_ASSERT_EQUAL_INT(2, static_cast<int>(d.sbits.size()));
  TEST_ASSERT_EQUAL_INT(2, d.sbits[0]);   // station 1 (bit 1) is on
  TEST_ASSERT_EQUAL_INT(0, d.sbits[1]);
  TEST_ASSERT_EQUAL_INT(3, static_cast<int>(d.ps.size()));
  TEST_ASSERT_EQUAL_INT(0, d.ps[0].pid);
  TEST_ASSERT_EQUAL_INT(99, d.ps[1].pid);
  TEST_ASSERT_EQUAL_INT(300, d.ps[1].rem);
  TEST_ASSERT_EQUAL_INT(1719719700, d.ps[1].start);
}

void test_parse_jc_missing_rssi() {
  // RSSI is optional (not present on some firmware versions).
  JcData d;
  TEST_ASSERT_TRUE(parse_jc(R"({"devt":1000,"sbits":[0],"ps":[]})", d));
  TEST_ASSERT_EQUAL_INT(0, d.rssi);
}

void test_parse_jc_malformed() {
  JcData d;
  TEST_ASSERT_FALSE(parse_jc("not json", d));
  TEST_ASSERT_FALSE(parse_jc("{}", d));    // missing devt
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

// ---------------------------------------------------------------------------

int main(int, char**) {
  UNITY_BEGIN();


  RUN_TEST(test_build_jn_url);
  RUN_TEST(test_build_jc_url);
  RUN_TEST(test_build_cm_url_run);
  RUN_TEST(test_build_cm_url_stop);
  RUN_TEST(test_build_cv_url);

  RUN_TEST(test_parse_jn_fields);
  RUN_TEST(test_parse_jn_malformed);

  RUN_TEST(test_parse_jc_fields);
  RUN_TEST(test_parse_jc_missing_rssi);
  RUN_TEST(test_parse_jc_malformed);

  RUN_TEST(test_parse_result_ok);
  RUN_TEST(test_parse_result_unauthorized);
  RUN_TEST(test_parse_result_not_permitted);
  RUN_TEST(test_parse_result_network_error);

  RUN_TEST(test_client_fetch_jn);
  RUN_TEST(test_client_fetch_jc);
  RUN_TEST(test_client_run_station);
  RUN_TEST(test_client_stop_station);
  RUN_TEST(test_client_stop_all);
  RUN_TEST(test_client_advance_off_then_on);
  RUN_TEST(test_client_advance_aborts_if_stop_fails);
  RUN_TEST(test_client_extend_off_then_on_same_sid);
  RUN_TEST(test_client_network_error_sets_disconnected);

  return UNITY_END();
}
