// Hardware-independent OpenSprinkler program schedule domain model.
//
// Program encoding mirrors /jp `pd[]` tuples:
//   [flag, days0, days1, [start0..3], [dur0..], name, [endr,from,to]]
// where flag bits encode enable/weather/oddeven/type/starttime_type/date-range,
// days0/days1 encode schedule matching by type, starttimes are int16_t encoded
// fixed/sunrise/sunset slots, durations are per-station seconds, and daterange
// uses date_encode(m,d)=(m<<5)+d.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace osp {

enum class ProgramType {
  Weekly = 0,
  SingleRun = 1,
  Monthly = 2,
  Interval = 3,
};

enum class OddEven {
  None = 0,
  Odd = 1,
  Even = 2,
};

struct Program {
  bool enabled = false;
  bool use_weather = false;
  OddEven oddeven = OddEven::None;
  ProgramType type = ProgramType::Weekly;
  bool starttime_type_fixed = false;
  bool en_daterange = false;

  int days0 = 0;
  int days1 = 0;
  std::array<int16_t, 4> starttimes = {0, 0, 0, 0};
  std::vector<int> durations;
  std::string name;
  std::array<int, 3> daterange = {0, 0, 0};

  int station_count() const;
  int total_seconds() const;
};

Program load_program(int flag,
                     int days0,
                     int days1,
                     const std::array<int16_t, 4>& starttimes,
                     const std::vector<int>& durations,
                     const std::string& name,
                     const std::array<int, 3>& daterange);

int decode_starttime(int16_t t, int sunrise_min, int sunset_min);
std::vector<int> day_start_minutes(const Program& p,
                                   int sunrise_min,
                                   int sunset_min);
bool day_matches(const Program& p, long local_epoch);

long next_run(const Program& p,
              long now_local_epoch,
              int sunrise_min,
              int sunset_min,
              long sentinel = -1);

enum class RunClass {
  Idle,
  ManualRun,
  ProgramRun,
};

struct ProgramPsEntry {
  int pid = 0;
  int rem = 0;
  long start = 0;
  int gid = 0;
};

struct ProgramQueueEntry {
  int sid = -1;
  int total_seconds = 0;
  int remaining_seconds = 0;
  bool started = false;
};

struct ProgramRunState {
  RunClass run_class = RunClass::Idle;
  int program_index = -1;  // pid-1 for scheduled programs, -1 otherwise
  std::vector<ProgramQueueEntry> queue;
  int current_sid = -1;
  int current_station_number = 0;  // 1-based within queue; 0 when none
  int station_count = 0;
  int total_remaining_seconds = 0;
};

ProgramRunState resolve_program_run_state(const std::vector<ProgramPsEntry>& ps,
                                          int nprogs,
                                          long now_local_epoch);

}  // namespace osp
