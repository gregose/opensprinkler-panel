#include <unity.h>

#include <string>
#include <vector>

#include "history_model.h"

using namespace osp;

void setUp() {}
void tearDown() {}

static LogEntry run(int pid, int sid, uint32_t duration, uint32_t end) {
  LogEntry entry;
  entry.pid = pid;
  entry.sid = sid;
  entry.duration_s = duration;
  entry.end_epoch = end;
  return entry;
}

static LogEntry event(const char* code, uint32_t duration, uint32_t end) {
  LogEntry entry;
  entry.pid = 0;
  entry.event_code = code;
  entry.duration_s = duration;
  entry.end_epoch = end;
  return entry;
}

static JpData programs() {
  JpData jp;
  Program program;
  program.name = "Morning Lawn";
  jp.programs.push_back(program);
  return jp;
}

void test_maps_program_station_and_program_names() {
  const std::vector<LogEntry> logs = {run(1, 0, 600, 1722580320)};
  const auto records =
      build_history_records(logs, {"Front Lawn"}, programs(), 1722600000, 120);
  TEST_ASSERT_EQUAL_INT(1, static_cast<int>(records.size()));
  TEST_ASSERT_EQUAL_STRING("Front Lawn", records[0].name.c_str());
  TEST_ASSERT_EQUAL_STRING("Morning Lawn", records[0].tag.c_str());
  TEST_ASSERT_EQUAL_UINT32(600, records[0].duration_s);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(HistoryKind::ProgramRun),
                        static_cast<int>(records[0].kind));
}

void test_uses_station_and_program_fallback_names() {
  const std::vector<LogEntry> logs = {run(2, 4, 60, 1722580320)};
  const auto records =
      build_history_records(logs, {"Only Station"}, programs(), 1722600000, 120);
  TEST_ASSERT_EQUAL_STRING("Station 5", records[0].name.c_str());
  TEST_ASSERT_EQUAL_STRING("Program 2", records[0].tag.c_str());
}

void test_classifies_manual_and_run_once() {
  const std::vector<LogEntry> logs = {
      run(99, 0, 45, 100), run(254, 1, 300, 200)};
  const auto records =
      build_history_records(logs, {"Front", "Back"}, programs(), 300, 120);
  TEST_ASSERT_EQUAL_INT(2, static_cast<int>(records.size()));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(HistoryKind::RunOnce),
                        static_cast<int>(records[0].kind));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(HistoryKind::ManualRun),
                        static_cast<int>(records[1].kind));
}

void test_maps_visible_events_and_hides_flow_and_watering_level() {
  const std::vector<LogEntry> logs = {
      event("s1", 1, 100), event("fl", 10, 200), event("s2", 0, 300),
      event("wl", 85, 400), event("rd", 86400, 500)};
  const auto records =
      build_history_records(logs, {}, programs(), 600, 120);
  TEST_ASSERT_EQUAL_INT(3, static_cast<int>(records.size()));
  TEST_ASSERT_EQUAL_STRING("Rain delay", records[0].name.c_str());
  TEST_ASSERT_EQUAL_INT(static_cast<int>(HistoryKind::RainDelay),
                        static_cast<int>(records[0].kind));
  TEST_ASSERT_EQUAL_STRING("Sensor 2", records[1].name.c_str());
  TEST_ASSERT_EQUAL_INT(static_cast<int>(HistoryKind::Sensor2),
                        static_cast<int>(records[1].kind));
  TEST_ASSERT_EQUAL_STRING("Sensor 1", records[2].name.c_str());
  TEST_ASSERT_EQUAL_INT(static_cast<int>(HistoryKind::Sensor1),
                        static_cast<int>(records[2].kind));
  TEST_ASSERT_EQUAL_UINT32(0, records[0].duration_s);
}

void test_formats_timestamp_tiers_from_controller_local_epoch() {
  TEST_ASSERT_EQUAL_STRING(
      "Today 6:32a",
      format_history_timestamp(1722580320, 1722600000).c_str());
  TEST_ASSERT_EQUAL_STRING(
      "Thu 8:15p",
      format_history_timestamp(1722543300, 1722600000).c_str());
  TEST_ASSERT_EQUAL_STRING(
      "Jul 25",
      format_history_timestamp(1721865600, 1722600000).c_str());
}

void test_returns_newest_first_and_skips_unknown_records() {
  const std::vector<LogEntry> logs = {
      run(1, 0, 10, 100), event("unknown", 0, 200), run(99, 1, 20, 300)};
  const auto records =
      build_history_records(logs, {"Old", "New"}, programs(), 400, 120);
  TEST_ASSERT_EQUAL_INT(2, static_cast<int>(records.size()));
  TEST_ASSERT_EQUAL_STRING("New", records[0].name.c_str());
  TEST_ASSERT_EQUAL_STRING("Old", records[1].name.c_str());
}

void test_caps_visible_records_after_filtering() {
  std::vector<LogEntry> logs;
  logs.push_back(event("fl", 1, 1));
  for (int i = 0; i < 125; ++i) {
    logs.push_back(run(99, 0, 10, static_cast<uint32_t>(i + 2)));
  }
  const auto records =
      build_history_records(logs, {"Front"}, programs(), 200, 120);
  TEST_ASSERT_EQUAL_INT(120, static_cast<int>(records.size()));
  TEST_ASSERT_EQUAL_STRING("Front", records.front().name.c_str());
  TEST_ASSERT_EQUAL_STRING("Front", records.back().name.c_str());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_maps_program_station_and_program_names);
  RUN_TEST(test_uses_station_and_program_fallback_names);
  RUN_TEST(test_classifies_manual_and_run_once);
  RUN_TEST(test_maps_visible_events_and_hides_flow_and_watering_level);
  RUN_TEST(test_formats_timestamp_tiers_from_controller_local_epoch);
  RUN_TEST(test_returns_newest_first_and_skips_unknown_records);
  RUN_TEST(test_caps_visible_records_after_filtering);
  return UNITY_END();
}
