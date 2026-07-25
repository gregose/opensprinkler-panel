// Native (host) unit tests for the hardware-independent program schedule model.
// Runs under `pio test -e native` — no board or network required.
#include <unity.h>

#include <array>
#include <vector>

#include "program_model.h"

using namespace osp;

void setUp() {}
void tearDown() {}

namespace {

constexpr long kDaySeconds = 86400;

long days_from_civil(int y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return static_cast<long>(era * 146097 + static_cast<int>(doe) - 719468);
}

long local_epoch(int y, unsigned m, unsigned d, int hh = 0, int mm = 0,
                 int ss = 0) {
  return days_from_civil(y, m, d) * kDaySeconds + hh * 3600 + mm * 60 + ss;
}

int make_flag(bool en, bool weather, int oddeven, int type, bool fixed,
              bool daterange) {
  return (en ? 1 : 0) | (weather ? (1 << 1) : 0) | ((oddeven & 0x3) << 2) |
         ((type & 0x3) << 4) | (fixed ? (1 << 6) : 0) |
         (daterange ? (1 << 7) : 0);
}

Program make_weekly_all_days(bool fixed = true) {
  return load_program(make_flag(true, false, 0, 0, fixed, false),
                      0b1111111,
                      0,
                      std::array<int16_t, 4>{600, 0, 0, 0},
                      std::vector<int>{60},
                      "p",
                      std::array<int, 3>{0, 0, 0});
}

}  // namespace

void test_load_program_decodes_flags_and_helpers() {
  Program p = load_program(make_flag(true, true, 2, 3, true, true),
                           5,
                           7,
                           std::array<int16_t, 4>{1, 2, 3, 4},
                           std::vector<int>{0, 30, 10},
                           "Irrigate",
                           std::array<int, 3>{1, 42, 88});

  TEST_ASSERT_TRUE(p.enabled);
  TEST_ASSERT_TRUE(p.use_weather);
  TEST_ASSERT_EQUAL_INT((int)OddEven::Even, (int)p.oddeven);
  TEST_ASSERT_EQUAL_INT((int)ProgramType::Interval, (int)p.type);
  TEST_ASSERT_TRUE(p.starttime_type_fixed);
  TEST_ASSERT_TRUE(p.en_daterange);
  TEST_ASSERT_EQUAL_INT(2, p.station_count());
  TEST_ASSERT_EQUAL_INT(40, p.total_seconds());
  TEST_ASSERT_EQUAL_INT(42, p.daterange[1]);
  TEST_ASSERT_EQUAL_INT(88, p.daterange[2]);
}

void test_day_matches_weekly_mapping_sunday_and_monday() {
  Program p = load_program(make_flag(true, false, 0, 0, true, false),
                           (1 << 0) | (1 << 6),
                           0,
                           std::array<int16_t, 4>{100, 0, 0, 0},
                           std::vector<int>{30},
                           "W",
                           std::array<int, 3>{0, 0, 0});

  TEST_ASSERT_TRUE(day_matches(p, local_epoch(2026, 7, 19, 12, 0)));   // Sunday
  TEST_ASSERT_TRUE(day_matches(p, local_epoch(2026, 7, 20, 12, 0)));   // Monday
  TEST_ASSERT_FALSE(day_matches(p, local_epoch(2026, 7, 21, 12, 0)));  // Tuesday
}

void test_day_matches_interval_and_single_run() {
  const long d = days_from_civil(2026, 7, 22);
  Program interval = load_program(make_flag(true, false, 0, 3, true, false),
                                  static_cast<int>(d % 3),
                                  3,
                                  std::array<int16_t, 4>{100, 0, 0, 0},
                                  std::vector<int>{30},
                                  "I",
                                  std::array<int, 3>{0, 0, 0});

  TEST_ASSERT_TRUE(day_matches(interval, local_epoch(2026, 7, 22, 10, 0)));
  TEST_ASSERT_FALSE(day_matches(interval, local_epoch(2026, 7, 23, 10, 0)));

  Program single = load_program(make_flag(true, false, 0, 1, true, false),
                                static_cast<int>((d >> 8) & 0xFF),
                                static_cast<int>(d & 0xFF),
                                std::array<int16_t, 4>{100, 0, 0, 0},
                                std::vector<int>{30},
                                "S",
                                std::array<int, 3>{0, 0, 0});

  TEST_ASSERT_TRUE(day_matches(single, local_epoch(2026, 7, 22, 0, 0)));
  TEST_ASSERT_FALSE(day_matches(single, local_epoch(2026, 7, 23, 0, 0)));
}

void test_day_matches_monthly_regular_and_last_day() {
  Program monthly_15 = load_program(make_flag(true, false, 0, 2, true, false),
                                    15,
                                    0,
                                    std::array<int16_t, 4>{100, 0, 0, 0},
                                    std::vector<int>{30},
                                    "M15",
                                    std::array<int, 3>{0, 0, 0});
  TEST_ASSERT_TRUE(day_matches(monthly_15, local_epoch(2026, 7, 15, 8, 0)));
  TEST_ASSERT_FALSE(day_matches(monthly_15, local_epoch(2026, 7, 14, 8, 0)));

  Program monthly_last = load_program(make_flag(true, false, 0, 2, true, false),
                                      0,
                                      0,
                                      std::array<int16_t, 4>{100, 0, 0, 0},
                                      std::vector<int>{30},
                                      "ML",
                                      std::array<int, 3>{0, 0, 0});
  TEST_ASSERT_TRUE(day_matches(monthly_last, local_epoch(2028, 2, 29, 8, 0)));
  TEST_ASSERT_FALSE(day_matches(monthly_last, local_epoch(2028, 2, 28, 8, 0)));
  TEST_ASSERT_TRUE(day_matches(monthly_last, local_epoch(2027, 2, 28, 8, 0)));
}

void test_day_matches_oddeven_rules() {
  Program odd = load_program(make_flag(true, false, 1, 0, true, false),
                             0b1111111,
                             0,
                             std::array<int16_t, 4>{100, 0, 0, 0},
                             std::vector<int>{30},
                             "O",
                             std::array<int, 3>{0, 0, 0});

  TEST_ASSERT_TRUE(day_matches(odd, local_epoch(2026, 3, 29, 8, 0)));
  TEST_ASSERT_FALSE(day_matches(odd, local_epoch(2026, 3, 31, 8, 0)));
  TEST_ASSERT_FALSE(day_matches(odd, local_epoch(2028, 2, 29, 8, 0)));

  Program even = load_program(make_flag(true, false, 2, 0, true, false),
                              0b1111111,
                              0,
                              std::array<int16_t, 4>{100, 0, 0, 0},
                              std::vector<int>{30},
                              "E",
                              std::array<int, 3>{0, 0, 0});

  TEST_ASSERT_TRUE(day_matches(even, local_epoch(2026, 7, 10, 8, 0)));
  TEST_ASSERT_FALSE(day_matches(even, local_epoch(2026, 7, 11, 8, 0)));
}

void test_day_matches_daterange_normal_and_wrap() {
  Program normal = load_program(make_flag(true, false, 0, 0, true, true),
                                0b1111111,
                                0,
                                std::array<int16_t, 4>{100, 0, 0, 0},
                                std::vector<int>{30},
                                "DN",
                                std::array<int, 3>{0, (5 << 5) + 10, (5 << 5) + 20});
  TEST_ASSERT_TRUE(day_matches(normal, local_epoch(2026, 5, 15, 8, 0)));
  TEST_ASSERT_FALSE(day_matches(normal, local_epoch(2026, 5, 9, 8, 0)));

  Program wrap = load_program(make_flag(true, false, 0, 0, true, true),
                              0b1111111,
                              0,
                              std::array<int16_t, 4>{100, 0, 0, 0},
                              std::vector<int>{30},
                              "DW",
                              std::array<int, 3>{0, (11 << 5) + 15, (2 << 5) + 10});
  TEST_ASSERT_TRUE(day_matches(wrap, local_epoch(2026, 12, 1, 8, 0)));
  TEST_ASSERT_TRUE(day_matches(wrap, local_epoch(2027, 1, 15, 8, 0)));
  TEST_ASSERT_FALSE(day_matches(wrap, local_epoch(2026, 6, 1, 8, 0)));
}

void test_decode_starttime_variants() {
  TEST_ASSERT_EQUAL_INT(123, decode_starttime(123, 360, 1080));
  TEST_ASSERT_EQUAL_INT(390, decode_starttime((1 << 14) | 30, 360, 1080));
  TEST_ASSERT_EQUAL_INT(340,
                        decode_starttime((1 << 14) | (1 << 12) | 20, 360, 1080));
  TEST_ASSERT_EQUAL_INT(0,
                        decode_starttime((1 << 14) | (1 << 12) | 30, 10, 1080));
  TEST_ASSERT_EQUAL_INT(1095, decode_starttime((1 << 13) | 15, 360, 1080));
  TEST_ASSERT_EQUAL_INT(1439,
                        decode_starttime((1 << 13) | 30, 360, 1430));
  TEST_ASSERT_EQUAL_INT(-1, decode_starttime((1 << 15) | 100, 360, 1080));
}

void test_day_start_minutes_fixed_and_repeating() {
  Program fixed = load_program(make_flag(true, false, 0, 0, true, false),
                               0,
                               0,
                               std::array<int16_t, 4>{100, static_cast<int16_t>(1 << 15),
                                                      static_cast<int16_t>(1 << 14),
                                                      static_cast<int16_t>((1 << 13) | 10)},
                               std::vector<int>{30},
                               "F",
                               std::array<int, 3>{0, 0, 0});
  std::vector<int> fs = day_start_minutes(fixed, 360, 1080);
  TEST_ASSERT_EQUAL_INT(3, (int)fs.size());
  TEST_ASSERT_EQUAL_INT(100, fs[0]);
  TEST_ASSERT_EQUAL_INT(360, fs[1]);
  TEST_ASSERT_EQUAL_INT(1090, fs[2]);

  Program repeating = load_program(make_flag(true, false, 0, 0, false, false),
                                   0,
                                   0,
                                   std::array<int16_t, 4>{100, 2, 30, 0},
                                   std::vector<int>{30},
                                   "R",
                                   std::array<int, 3>{0, 0, 0});
  std::vector<int> rs = day_start_minutes(repeating, 360, 1080);
  TEST_ASSERT_EQUAL_INT(3, (int)rs.size());
  TEST_ASSERT_EQUAL_INT(100, rs[0]);
  TEST_ASSERT_EQUAL_INT(130, rs[1]);
  TEST_ASSERT_EQUAL_INT(160, rs[2]);
}

void test_next_run_weekly_and_today_tomorrow_boundary() {
  Program monday = load_program(make_flag(true, false, 0, 0, true, false),
                                (1 << 0),
                                0,
                                std::array<int16_t, 4>{60, static_cast<int16_t>(1 << 15),
                                                       static_cast<int16_t>(1 << 15),
                                                       static_cast<int16_t>(1 << 15)},
                                std::vector<int>{30},
                                "Mon",
                                std::array<int, 3>{0, 0, 0});

  const long sunday_10 = local_epoch(2026, 7, 19, 10, 0);
  TEST_ASSERT_EQUAL_INT64(local_epoch(2026, 7, 20, 1, 0),
                          next_run(monday, sunday_10, 360, 1080));

  Program daily = make_weekly_all_days(true);
  daily.starttimes = {600,
                      static_cast<int16_t>(1 << 15),
                      static_cast<int16_t>(1 << 15),
                      static_cast<int16_t>(1 << 15)};

  TEST_ASSERT_EQUAL_INT64(local_epoch(2026, 7, 22, 10, 0),
                          next_run(daily, local_epoch(2026, 7, 22, 9, 0), 360, 1080));
  TEST_ASSERT_EQUAL_INT64(local_epoch(2026, 7, 23, 10, 0),
                          next_run(daily, local_epoch(2026, 7, 22, 10, 0), 360, 1080));
}

void test_next_run_interval_disabled_and_daterange_wrap() {
  const long now = local_epoch(2026, 7, 22, 12, 0);
  const long day = days_from_civil(2026, 7, 22);
  Program interval = load_program(make_flag(true, false, 0, 3, true, false),
                                  static_cast<int>((day + 1) % 2),
                                  2,
                                  std::array<int16_t, 4>{120, static_cast<int16_t>(1 << 15),
                                                         static_cast<int16_t>(1 << 15),
                                                         static_cast<int16_t>(1 << 15)},
                                  std::vector<int>{30},
                                  "I",
                                  std::array<int, 3>{0, 0, 0});
  TEST_ASSERT_EQUAL_INT64(local_epoch(2026, 7, 23, 2, 0),
                          next_run(interval, now, 360, 1080));

  Program disabled = interval;
  disabled.enabled = false;
  TEST_ASSERT_EQUAL_INT64(-1, next_run(disabled, now, 360, 1080));

  Program wrap = make_weekly_all_days(true);
  wrap.en_daterange = true;
  wrap.daterange = {0, (11 << 5) + 15, (2 << 5) + 10};
  wrap.starttimes = {100, static_cast<int16_t>(1 << 15), static_cast<int16_t>(1 << 15),
                     static_cast<int16_t>(1 << 15)};
  TEST_ASSERT_EQUAL_INT64(local_epoch(2026, 11, 15, 1, 40),
                          next_run(wrap, local_epoch(2026, 6, 1, 0, 0), 360, 1080));
}

void test_run_state_classification_idle_manual_program_and_run_once() {
  std::vector<ProgramPsEntry> idle(3);
  ProgramRunState s_idle = resolve_program_run_state(idle, 4, 1000);
  TEST_ASSERT_EQUAL_INT((int)RunClass::Idle, (int)s_idle.run_class);

  std::vector<ProgramPsEntry> manual(2);
  manual[1].pid = 99;
  manual[1].rem = 50;
  ProgramRunState s_manual = resolve_program_run_state(manual, 4, 1000);
  TEST_ASSERT_EQUAL_INT((int)RunClass::ManualRun, (int)s_manual.run_class);

  std::vector<ProgramPsEntry> prog(4);
  prog[0] = ProgramPsEntry{2, 40, 900, 0};
  prog[1] = ProgramPsEntry{2, 30, 1100, 0};
  prog[2] = ProgramPsEntry{2, 20, 800, 0};
  ProgramRunState s_prog = resolve_program_run_state(prog, 4, 1000);
  TEST_ASSERT_EQUAL_INT((int)RunClass::ProgramRun, (int)s_prog.run_class);
  TEST_ASSERT_EQUAL_INT(1, s_prog.program_index);
  TEST_ASSERT_EQUAL_INT(3, (int)s_prog.queue.size());
  TEST_ASSERT_EQUAL_INT(2, s_prog.queue[0].sid);
  TEST_ASSERT_EQUAL_INT(0, s_prog.queue[1].sid);
  TEST_ASSERT_EQUAL_INT(1, s_prog.queue[2].sid);
  TEST_ASSERT_EQUAL_INT(220, s_prog.queue[0].total_seconds);
  TEST_ASSERT_EQUAL_INT(140, s_prog.queue[1].total_seconds);
  TEST_ASSERT_EQUAL_INT(30, s_prog.queue[2].total_seconds);
  TEST_ASSERT_EQUAL_INT(0, s_prog.current_sid);
  TEST_ASSERT_EQUAL_INT(2, s_prog.current_station_number);
  TEST_ASSERT_EQUAL_INT(3, s_prog.station_count);
  TEST_ASSERT_EQUAL_INT(90, s_prog.total_remaining_seconds);

  std::vector<ProgramPsEntry> run_once(2);
  run_once[0] = ProgramPsEntry{254, 10, 990, 0};
  ProgramRunState s_run_once = resolve_program_run_state(run_once, 4, 1000);
  TEST_ASSERT_EQUAL_INT((int)RunClass::ProgramRun, (int)s_run_once.run_class);
  TEST_ASSERT_EQUAL_INT(-1, s_run_once.program_index);
}

// With the program's full ordered station list supplied, the queue spans every
// station (including finished ones dropped from ps), station_count is the fixed
// total, and current_station_number counts up through it.
void test_run_state_full_station_set_counts_up_and_marks_done() {
  // Program index 1 (pid=2), 4 stations. Stations 0,1 finished (dropped from
  // ps); station 2 running; station 3 upcoming (future start).
  std::vector<ProgramPsEntry> ps(4);
  ps[2] = ProgramPsEntry{2, 120, 950, 0};   // started (start<=now)
  ps[3] = ProgramPsEntry{2, 180, 1070, 0};  // not started yet

  std::vector<ProgramStation> stations = {
      {0, 300}, {1, 300}, {2, 240}, {3, 180}};

  ProgramRunState s =
      resolve_program_run_state(ps, 4, 1000, &stations);

  TEST_ASSERT_EQUAL_INT((int)RunClass::ProgramRun, (int)s.run_class);
  TEST_ASSERT_EQUAL_INT(4, s.station_count);
  TEST_ASSERT_EQUAL_INT(4, (int)s.queue.size());

  // Finished stations included and flagged done.
  TEST_ASSERT_TRUE(s.queue[0].done);
  TEST_ASSERT_TRUE(s.queue[1].done);
  TEST_ASSERT_EQUAL_INT(0, s.queue[0].remaining_seconds);
  TEST_ASSERT_EQUAL_INT(300, s.queue[0].total_seconds);

  // Current is the running station, counted at its absolute position (3 of 4).
  TEST_ASSERT_FALSE(s.queue[2].done);
  TEST_ASSERT_TRUE(s.queue[2].started);
  TEST_ASSERT_EQUAL_INT(2, s.current_sid);
  TEST_ASSERT_EQUAL_INT(3, s.current_station_number);

  // Upcoming station present, not started, not done.
  TEST_ASSERT_FALSE(s.queue[3].done);
  TEST_ASSERT_FALSE(s.queue[3].started);
  TEST_ASSERT_EQUAL_INT(180, s.queue[3].remaining_seconds);

  TEST_ASSERT_EQUAL_INT(300, s.total_remaining_seconds);  // 120 + 180
}

// While paused, the controller pushes every start time into the future so no
// station reports as started; the current station falls back to the first
// not-yet-done station with time left, at its absolute position.
void test_run_state_full_station_set_paused_picks_pending_station() {
  std::vector<ProgramPsEntry> ps(4);
  ps[2] = ProgramPsEntry{2, 120, 1200, 0};  // paused → start in the future
  ps[3] = ProgramPsEntry{2, 180, 1380, 0};

  std::vector<ProgramStation> stations = {
      {0, 300}, {1, 300}, {2, 240}, {3, 180}};

  ProgramRunState s =
      resolve_program_run_state(ps, 4, 1000, &stations);

  TEST_ASSERT_EQUAL_INT(4, s.station_count);
  TEST_ASSERT_FALSE(s.queue[2].started);
  TEST_ASSERT_EQUAL_INT(2, s.current_sid);
  TEST_ASSERT_EQUAL_INT(3, s.current_station_number);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_load_program_decodes_flags_and_helpers);
  RUN_TEST(test_day_matches_weekly_mapping_sunday_and_monday);
  RUN_TEST(test_day_matches_interval_and_single_run);
  RUN_TEST(test_day_matches_monthly_regular_and_last_day);
  RUN_TEST(test_day_matches_oddeven_rules);
  RUN_TEST(test_day_matches_daterange_normal_and_wrap);
  RUN_TEST(test_decode_starttime_variants);
  RUN_TEST(test_day_start_minutes_fixed_and_repeating);
  RUN_TEST(test_next_run_weekly_and_today_tomorrow_boundary);
  RUN_TEST(test_next_run_interval_disabled_and_daterange_wrap);
  RUN_TEST(test_run_state_classification_idle_manual_program_and_run_once);
  RUN_TEST(test_run_state_full_station_set_counts_up_and_marks_done);
  RUN_TEST(test_run_state_full_station_set_paused_picks_pending_station);
  return UNITY_END();
}
