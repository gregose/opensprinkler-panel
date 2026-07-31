#include "program_model.h"

#include <algorithm>
#include <cstdio>
#include <string>

namespace osp {
namespace {

constexpr long kDaySeconds = 86400;

long floor_div(long a, long b) {
  long q = a / b;
  const long r = a % b;
  if (r != 0 && ((r > 0) != (b > 0))) --q;
  return q;
}

long positive_mod(long a, long b) {
  long r = a % b;
  if (r < 0) r += b;
  return r;
}

struct CivilDay {
  int year;
  int month;
  int day;
  int weekday_sun0;
  long epoch_day;
};

CivilDay civil_from_epoch_day(long z) {
  const long z2 = z + 719468;
  const long era = (z2 >= 0 ? z2 : z2 - 146096) / 146097;
  const unsigned doe = static_cast<unsigned>(z2 - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  int y = static_cast<int>(yoe) + static_cast<int>(era) * 400;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  const unsigned d = doy - (153 * mp + 2) / 5 + 1;
  const unsigned m = mp + (mp < 10 ? 3 : -9);
  y += (m <= 2);
  const int weekday = static_cast<int>(positive_mod(z + 4, 7));
  return CivilDay{y, static_cast<int>(m), static_cast<int>(d), weekday, z};
}

bool is_leap_year(int year) {
  if ((year % 4) != 0) return false;
  if ((year % 100) != 0) return true;
  return (year % 400) == 0;
}

int last_day_of_month(int year, int month) {
  static const int kMonthDays[12] = {31, 28, 31, 30, 31, 30,
                                     31, 31, 30, 31, 30, 31};
  if (month == 2 && is_leap_year(year)) return 29;
  return kMonthDays[month - 1];
}

bool date_range_matches(const Program& p, const CivilDay& day) {
  if (!p.en_daterange) return true;
  const int currdate = (day.month << 5) + day.day;
  const int from = p.daterange[1];
  const int to = p.daterange[2];
  if (from <= to) return currdate >= from && currdate <= to;
  return !(currdate > to && currdate < from);
}

bool base_day_match(const Program& p, const CivilDay& day) {
  switch (p.type) {
    case ProgramType::Weekly: {
      const int wd = (day.weekday_sun0 + 6) % 7;
      return (p.days0 & (1 << wd)) != 0;
    }
    case ProgramType::SingleRun:
      return ((p.days0 << 8) + p.days1) == day.epoch_day;
    case ProgramType::Monthly: {
      const int d = p.days0 & 0x1F;
      if (d == 0) return day.day == last_day_of_month(day.year, day.month);
      return day.day == d;
    }
    case ProgramType::Interval:
      if (p.days1 <= 0) return false;
      return positive_mod(day.epoch_day, p.days1) == positive_mod(p.days0, p.days1);
  }
  return false;
}

bool oddeven_match(const Program& p, const CivilDay& day) {
  const int d = day.day;
  if (p.oddeven == OddEven::Even) {
    return (d % 2) == 0;
  }
  if (p.oddeven == OddEven::Odd) {
    if (day.month == 2 && d == 29) return false;
    if (d == 31) return false;
    return (d % 2) == 1;
  }
  return true;
}

int pick_pid(const std::vector<ProgramPsEntry>& ps, int nprogs, RunClass& cls) {
  for (const auto& e : ps) {
    if (e.pid == 99) {
      cls = RunClass::ManualRun;
      return 99;
    }
  }
  for (const auto& e : ps) {
    if (e.pid == 254 || (e.pid >= 1 && e.pid <= nprogs)) {
      cls = RunClass::ProgramRun;
      return e.pid;
    }
  }
  cls = RunClass::Idle;
  return 0;
}

}  // namespace

int Program::station_count() const {
  int n = 0;
  for (int d : durations) {
    if (d > 0) ++n;
  }
  return n;
}

int Program::total_seconds() const {
  int sum = 0;
  for (int d : durations) {
    sum += d;
  }
  return sum;
}

Program load_program(int flag,
                     int days0,
                     int days1,
                     const std::array<int16_t, 4>& starttimes,
                     const std::vector<int>& durations,
                     const std::string& name,
                     const std::array<int, 3>& daterange) {
  Program p;
  p.enabled = (flag & (1 << 0)) != 0;
  p.use_weather = (flag & (1 << 1)) != 0;
  p.oddeven = static_cast<OddEven>((flag >> 2) & 0x3);
  p.type = static_cast<ProgramType>((flag >> 4) & 0x3);
  p.starttime_type_fixed = ((flag >> 6) & 0x1) != 0;
  p.en_daterange = ((flag >> 7) & 0x1) != 0;
  p.days0 = days0;
  p.days1 = days1;
  p.starttimes = starttimes;
  p.durations = durations;
  p.name = name;
  p.daterange = daterange;
  return p;
}

int decode_starttime(int16_t t, int sunrise_min, int sunset_min) {
  if ((t >> 15) & 1) return -1;
  int offset = t & 0x7FF;
  if ((t >> 12) & 1) offset = -offset;
  int decoded = t;
  if ((t >> 14) & 1) {
    decoded = sunrise_min + offset;
    if (decoded < 0) decoded = 0;
  } else if ((t >> 13) & 1) {
    decoded = sunset_min + offset;
    if (decoded >= 1440) decoded = 1439;
  }
  return decoded;
}

std::vector<int> day_start_minutes(const Program& p,
                                   int sunrise_min,
                                   int sunset_min) {
  std::vector<int> out;
  if (p.starttime_type_fixed) {
    for (int i = 0; i < 4; ++i) {
      const int t = decode_starttime(p.starttimes[i], sunrise_min, sunset_min);
      if (t >= 0) out.push_back(t);
    }
    std::sort(out.begin(), out.end());
    return out;
  }

  const int start = decode_starttime(p.starttimes[0], sunrise_min, sunset_min);
  if (start < 0) return out;

  int count = p.starttimes[1];
  const int every = p.starttimes[2];
  if (count < 0) count = 0;
  if (every <= 0) count = 0;
  out.reserve(static_cast<std::size_t>(count) + 1);
  for (int i = 0; i <= count; ++i) {
    const int t = start + i * every;
    if (t >= 0 && t < 1440) out.push_back(t);
  }
  return out;
}

bool day_matches(const Program& p, long local_epoch) {
  const long epoch_day = floor_div(local_epoch, kDaySeconds);
  const CivilDay day = civil_from_epoch_day(epoch_day);
  if (!date_range_matches(p, day)) return false;
  if (!base_day_match(p, day)) return false;
  return oddeven_match(p, day);
}

long next_run(const Program& p,
              long now_local_epoch,
              int sunrise_min,
              int sunset_min,
              long sentinel) {
  if (!p.enabled) return sentinel;
  const long now_day = floor_div(now_local_epoch, kDaySeconds);
  for (long off = 0; off <= 400; ++off) {
    const long day = now_day + off;
    const long day_start = day * kDaySeconds;
    if (!day_matches(p, day_start)) continue;

    const std::vector<int> starts = day_start_minutes(p, sunrise_min, sunset_min);
    for (int minute : starts) {
      const long ts = day_start + static_cast<long>(minute) * 60;
      if (ts > now_local_epoch) return ts;
    }
  }
  return sentinel;
}

ProgramRunState resolve_program_run_state(
    const std::vector<ProgramPsEntry>& ps,
    int nprogs,
    long now_local_epoch,
    const std::vector<ProgramStation>* program_stations) {
  ProgramRunState state;
  RunClass cls = RunClass::Idle;
  const int pid = pick_pid(ps, nprogs, cls);
  state.run_class = cls;

  if (cls != RunClass::ProgramRun) return state;

  state.program_index = (pid == 254) ? -1 : (pid - 1);

  // ---- Full-set path: reconstruct the whole program, including stations
  // that have already finished (and were dropped from the live ps[]). -------
  if (program_stations != nullptr && !program_stations->empty()) {
    int current_idx = -1;
    for (const auto& def : *program_stations) {
      ProgramQueueEntry q;
      q.sid = def.sid;
      q.total_seconds = def.total_seconds;

      const ProgramPsEntry* in = nullptr;
      if (def.sid >= 0 && def.sid < static_cast<int>(ps.size()) &&
          ps[def.sid].pid == pid) {
        in = &ps[def.sid];
      }

      if (in != nullptr && in->rem > 0) {
        q.remaining_seconds = in->rem;
        q.started = in->start != 0 && in->start <= now_local_epoch;
        q.done = false;
      } else {
        // Not in the live queue for this pid (or drained) → already ran.
        q.remaining_seconds = 0;
        q.started = true;
        q.done = true;
      }

      state.total_remaining_seconds += q.remaining_seconds;
      state.queue.push_back(q);
    }

    // Current station = the running one (started, time left). While paused the
    // controller pushes all starts into the future, so nothing reports as
    // started; fall back to the first not-yet-done station with time left.
    for (int i = 0; i < static_cast<int>(state.queue.size()); ++i) {
      const auto& q = state.queue[i];
      if (q.started && !q.done && q.remaining_seconds > 0) { current_idx = i; break; }
    }
    if (current_idx < 0) {
      for (int i = 0; i < static_cast<int>(state.queue.size()); ++i) {
        if (!state.queue[i].done && state.queue[i].remaining_seconds > 0) {
          current_idx = i;
          break;
        }
      }
    }

    state.station_count = static_cast<int>(state.queue.size());
    if (current_idx >= 0) {
      state.current_sid = state.queue[current_idx].sid;
      state.current_station_number = current_idx + 1;
    }
    return state;
  }

  // ---- Legacy path: reconstruct only from the live ps[] entries. ----------
  struct Ordered {
    ProgramQueueEntry e;
    long start;
  };
  std::vector<Ordered> ordered;

  for (int sid = 0; sid < static_cast<int>(ps.size()); ++sid) {
    const ProgramPsEntry& in = ps[sid];
    if (in.pid != pid) continue;

    ProgramQueueEntry q;
    q.sid = sid;
    q.remaining_seconds = std::max(0, in.rem);
    q.started = in.start != 0 && in.start <= now_local_epoch;
    if (q.started) {
      const long elapsed = std::max(0L, now_local_epoch - in.start);
      q.total_seconds = q.remaining_seconds + static_cast<int>(elapsed);
    } else {
      q.total_seconds = q.remaining_seconds;
    }

    ordered.push_back(Ordered{q, in.start});
  }

  std::stable_sort(ordered.begin(), ordered.end(),
                   [](const Ordered& a, const Ordered& b) {
                     return a.start < b.start;
                   });

  int current_idx = -1;
  long latest_started = -1;
  for (int i = 0; i < static_cast<int>(ordered.size()); ++i) {
    state.queue.push_back(ordered[i].e);
    state.total_remaining_seconds += ordered[i].e.remaining_seconds;
    if (ordered[i].e.started && ordered[i].e.remaining_seconds > 0 &&
        ordered[i].start >= latest_started) {
      latest_started = ordered[i].start;
      current_idx = i;
    }
  }

  state.station_count = static_cast<int>(state.queue.size());
  if (current_idx >= 0) {
    state.current_sid = state.queue[current_idx].sid;
    state.current_station_number = current_idx + 1;
  }
  return state;
}

std::string program_run_eyebrow(int program_index,
                                int current_station_number,
                                int station_count) {
  if (station_count <= 0) return "STATION";
  char buf[24];
  if (program_index < 0) {
    // Unidentified external run: honest remaining count, no false "of M".
    snprintf(buf, sizeof(buf), "%d STATION%s LEFT", station_count,
             station_count == 1 ? "" : "S");
    return buf;
  }
  if (current_station_number > 0) {
    snprintf(buf, sizeof(buf), "STATION %d OF %d", current_station_number,
             station_count);
    return buf;
  }
  return "FINISHING";
}

}  // namespace osp
