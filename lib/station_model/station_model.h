// Hardware-independent OpenSprinkler station domain model.
//
// Everything here is pure C++ (no Arduino, no network) so it builds and runs
// under the PlatformIO `native` test environment. It encodes the behaviour that
// docs/01-ux-spec.md and docs/02-opensprinkler-api.md specify:
//   - decode the controller's per-board station bitmasks (/jn, /jc),
//   - filter disabled + master/pump stations out of the grid,
//   - lay out the station grid (1 row <=12, 2 rows 13-24, without shrinking),
//   - Prev/Advance navigation that wraps and skips non-runnable stations,
//   - auto-advance that stops after the last station,
//   - map a Wi-Fi RSSI to 0..4 signal bars.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace osp {

// Test whether station `sid` is set in a controller per-board bitmask, where
// each byte is one 8-station board, LSB = the lowest station on that board
// (the encoding used by stn_dis / masop / masop2 / sbits). Out-of-range sids
// and bytes read as `false`.
bool board_bit_set(const std::vector<uint8_t>& boards, int sid);

struct Station {
  int sid = -1;   // 0-based controller station id
  std::string name;
  bool disabled = false;
  bool master = false;
  bool runnable() const { return !disabled && !master; }
};

struct GridLayout {
  int rows = 0;
  int cols = 0;
};

// Grid layout for `n` runnable stations: rows = ceil(n/12) (so 13-24 -> 2 rows),
// cols = ceil(n/rows). n <= 0 -> {0,0}.
GridLayout grid_layout(int n);

// Wi-Fi RSSI (dBm) -> 0..4 bars, per docs/01 §top bar.
int rssi_to_bars(int rssi_dbm);

// Bars to actually light on screen: a connected link always shows at least 1
// bar so users can distinguish "weak signal" from "disconnected". Empty (0)
// means disconnected only.
//   connected=true  → max(1, clamp(quality, 0, 4))
//   connected=false → 0
int display_bars(int quality, bool connected);

class StationModel {
 public:
  // Build the model from the station configuration. `stn_dis` is the raw
  // per-board disabled bitmask (any length; empty = none disabled). `mas` and
  // `mas2` are the master station indices from /jo (1-based; 0 = none) — the
  // only stations treated as masters and omitted from the grid.
  //
  // NOTE: do NOT pass the /jn `masop`/`masop2` bitmasks here. Those are master
  // *association* masks (which stations open the master valve when they run);
  // using them as master identity wrongly filters out every pump-fed zone.
  void load(const std::vector<std::string>& names,
            const std::vector<uint8_t>& stn_dis,
            int mas, int mas2);

  const std::vector<Station>& stations() const { return stations_; }

  // The runnable stations' sids, in station order — this is the grid order and
  // the Prev/Advance navigation order.
  const std::vector<int>& runnable_sids() const { return runnable_; }
  int runnable_count() const { return static_cast<int>(runnable_.size()); }

  GridLayout layout() const { return grid_layout(runnable_count()); }

  // Position of `sid` within the runnable list, or -1 if not runnable.
  int runnable_index(int sid) const;

  // Manual Advance: next runnable station after `current_sid`, wrapping past the
  // last back to the first. If `current_sid` isn't runnable, returns the first
  // runnable station. -1 when there are no runnable stations.
  int next_sid(int current_sid) const;

  // Manual Prev: previous runnable station, wrapping from the first to the last.
  int prev_sid(int current_sid) const;

  // Auto-advance: the next runnable station after `current_sid`, or -1 when
  // `current_sid` is the last runnable station (an automatic pass stops after
  // the last station rather than looping). -1 also if `current_sid` isn't
  // runnable or there are none.
  int auto_next_sid(int current_sid) const;

 private:
  std::vector<Station> stations_;
  std::vector<int> runnable_;
};

}  // namespace osp
