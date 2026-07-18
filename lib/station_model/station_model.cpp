#include "station_model.h"

namespace osp {

bool board_bit_set(const std::vector<uint8_t>& boards, int sid) {
  if (sid < 0) return false;
  const std::size_t byte = static_cast<std::size_t>(sid) >> 3;
  if (byte >= boards.size()) return false;
  return (boards[byte] >> (sid & 7)) & 1;
}

GridLayout grid_layout(int n) {
  if (n <= 0) return GridLayout{0, 0};
  const int rows = (n + 11) / 12;       // ceil(n/12): 1..12->1, 13..24->2
  const int cols = (n + rows - 1) / rows;  // ceil(n/rows)
  return GridLayout{rows, cols};
}

int rssi_to_bars(int rssi_dbm) {
  if (rssi_dbm >= -55) return 4;
  if (rssi_dbm >= -65) return 3;
  if (rssi_dbm >= -72) return 2;
  if (rssi_dbm >= -82) return 1;
  return 0;
}

void StationModel::load(const std::vector<std::string>& names,
                        const std::vector<uint8_t>& stn_dis,
                        const std::vector<uint8_t>& masop,
                        const std::vector<uint8_t>& masop2) {
  stations_.clear();
  runnable_.clear();
  stations_.reserve(names.size());

  for (int sid = 0; sid < static_cast<int>(names.size()); ++sid) {
    Station s;
    s.sid = sid;
    s.name = names[sid];
    s.disabled = board_bit_set(stn_dis, sid);
    s.master = board_bit_set(masop, sid) || board_bit_set(masop2, sid);
    stations_.push_back(s);
    if (s.runnable()) runnable_.push_back(sid);
  }
}

int StationModel::runnable_index(int sid) const {
  for (int i = 0; i < static_cast<int>(runnable_.size()); ++i) {
    if (runnable_[i] == sid) return i;
  }
  return -1;
}

int StationModel::next_sid(int current_sid) const {
  if (runnable_.empty()) return -1;
  const int idx = runnable_index(current_sid);
  if (idx < 0) return runnable_.front();
  const int n = static_cast<int>(runnable_.size());
  return runnable_[(idx + 1) % n];
}

int StationModel::prev_sid(int current_sid) const {
  if (runnable_.empty()) return -1;
  const int idx = runnable_index(current_sid);
  if (idx < 0) return runnable_.back();
  const int n = static_cast<int>(runnable_.size());
  return runnable_[(idx - 1 + n) % n];
}

int StationModel::auto_next_sid(int current_sid) const {
  const int idx = runnable_index(current_sid);
  if (idx < 0) return -1;
  if (idx == static_cast<int>(runnable_.size()) - 1) return -1;  // stop after last
  return runnable_[idx + 1];
}

}  // namespace osp
