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
  bool done = false;  // station already completed (no longer in the live /jc queue)
};

// One station of a program's definition (from /jp durations), in station-id
// order. Used to reconstruct the *full* run queue — including already-completed
// stations, which the controller drops from /jc `ps[]` as they finish.
struct ProgramStation {
  int sid = 0;
  int total_seconds = 0;
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

// Resolve the controller's current run state from /jc `ps[]`.
//
// When `program_stations` is supplied (the running program's full ordered
// station list, from /jp), the returned queue spans *every* station in the
// program — completed ones (dropped from `ps`) are included with done=true —
// so `station_count` is the fixed total M and `current_station_number` counts
// up 1..M as the run progresses. When null, the queue is reconstructed from
// the live `ps[]` entries only (legacy behaviour).
ProgramRunState resolve_program_run_state(
    const std::vector<ProgramPsEntry>& ps,
    int nprogs,
    long now_local_epoch,
    const std::vector<ProgramStation>* program_stations = nullptr);

// Resolve a manual station queue from the live /jc `ps[]` entries.
//
// When any entry has pid=99, the returned state contains every live manual
// station in start-time order, with program_index=-1. Otherwise the state
// retains the detected run class and has an empty queue.
ProgramRunState resolve_manual_queue_state(
    const std::vector<ProgramPsEntry>& ps,
    long now_local_epoch);

// Eyebrow text for the program-run screen's left status column.
//   Identified program (program_index >= 0): "STATION N OF M" - position
//     through the full ordered station set (M fixed).
//   Unidentified/external run (program_index < 0): we do not know the full set
//     or our place in it, so report the honest remaining-station count instead
//     of a misleading "N OF M": "N STATIONS LEFT" (singular "1 STATION LEFT").
//     station_count here is the number of stations still queued, the currently
//     running one included.
//   No stations queued: "STATION".
std::string program_run_eyebrow(int program_index,
                                int current_station_number,
                                int station_count);

}  // namespace osp
