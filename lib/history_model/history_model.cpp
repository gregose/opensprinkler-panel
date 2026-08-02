#include "history_model.h"

#include <cstdio>

namespace osp {
namespace {

struct CivilDate {
  int month;
  int day;
};

CivilDate civil_date_from_epoch_day(int64_t epoch_day) {
  const int64_t z = epoch_day + 719468;
  const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = static_cast<unsigned>(z - era * 146097);
  const unsigned yoe =
      (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  const unsigned day = doy - (153 * mp + 2) / 5 + 1;
  const unsigned month = mp + (mp < 10 ? 3 : -9);
  return CivilDate{static_cast<int>(month), static_cast<int>(day)};
}

int positive_mod(int64_t value, int divisor) {
  int result = static_cast<int>(value % divisor);
  if (result < 0) result += divisor;
  return result;
}

std::string numbered_fallback(const char* prefix, int number) {
  char buf[32];
  snprintf(buf, sizeof(buf), "%s %d", prefix, number);
  return buf;
}

std::string station_name(const std::vector<std::string>& station_names,
                         int sid) {
  if (sid >= 0 && sid < static_cast<int>(station_names.size()) &&
      !station_names[sid].empty()) {
    return station_names[sid];
  }
  return numbered_fallback("Station", sid + 1);
}

std::string program_name(const JpData& programs, int pid) {
  const int index = pid - 1;
  if (index >= 0 && index < static_cast<int>(programs.programs.size()) &&
      !programs.programs[index].name.empty()) {
    return programs.programs[index].name;
  }
  return numbered_fallback("Program", pid);
}

bool map_entry(const LogEntry& log,
               const std::vector<std::string>& station_names,
               const JpData& programs,
               uint32_t now_local_epoch,
               HistoryRecord& out) {
  out.duration_s = log.duration_s;
  out.when = format_history_timestamp(log.end_epoch, now_local_epoch);

  if (log.pid == 0) {
    out.duration_s = 0;
    if (log.event_code == "rd") {
      out.name = "Rain delay";
      out.kind = HistoryKind::RainDelay;
      return true;
    }
    if (log.event_code == "s1") {
      out.name = "Sensor 1";
      out.kind = HistoryKind::Sensor1;
      return true;
    }
    if (log.event_code == "s2") {
      out.name = "Sensor 2";
      out.kind = HistoryKind::Sensor2;
      return true;
    }
    return false;
  }

  out.name = station_name(station_names, log.sid);
  if (log.pid == 99) {
    out.kind = HistoryKind::ManualRun;
    return true;
  }
  if (log.pid == 254) {
    out.kind = HistoryKind::RunOnce;
    return true;
  }
  if (log.pid >= 1 && log.pid <= 250) {
    out.kind = HistoryKind::ProgramRun;
    out.tag = program_name(programs, log.pid);
    return true;
  }
  return false;
}

}  // namespace

std::string format_history_timestamp(uint32_t end_epoch,
                                     uint32_t now_local_epoch) {
  static const char* kWeekdays[] = {
      "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
  static const char* kMonths[] = {
      "Jan", "Feb", "Mar", "Apr", "May", "Jun",
      "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

  const int64_t end_day = static_cast<int64_t>(end_epoch) / 86400;
  const int64_t now_day = static_cast<int64_t>(now_local_epoch) / 86400;
  const int64_t day_age = now_day - end_day;
  char buf[32];

  if (day_age >= 0 && day_age <= 6) {
    const uint32_t seconds_in_day = end_epoch % 86400;
    const int hour24 = static_cast<int>(seconds_in_day / 3600);
    const int minute = static_cast<int>((seconds_in_day % 3600) / 60);
    const int hour12 = (hour24 % 12 == 0) ? 12 : hour24 % 12;
    const char suffix = hour24 < 12 ? 'a' : 'p';
    if (day_age == 0) {
      snprintf(buf, sizeof(buf), "Today %d:%02d%c", hour12, minute, suffix);
    } else {
      const int weekday = positive_mod(end_day + 4, 7);
      snprintf(buf, sizeof(buf), "%s %d:%02d%c",
               kWeekdays[weekday], hour12, minute, suffix);
    }
    return buf;
  }

  const CivilDate date = civil_date_from_epoch_day(end_day);
  snprintf(buf, sizeof(buf), "%s %d", kMonths[date.month - 1], date.day);
  return buf;
}

std::vector<HistoryRecord> build_history_records(
    const std::vector<LogEntry>& logs,
    const std::vector<std::string>& station_names,
    const JpData& programs,
    uint32_t now_local_epoch,
    std::size_t max_records) {
  std::vector<HistoryRecord> records;
  records.reserve(logs.size() < max_records ? logs.size() : max_records);
  for (auto it = logs.rbegin();
       it != logs.rend() && records.size() < max_records; ++it) {
    HistoryRecord record;
    if (map_entry(*it, station_names, programs, now_local_epoch, record)) {
      records.push_back(std::move(record));
    }
  }
  return records;
}

}  // namespace osp
