// Hardware-independent mapping from OpenSprinkler /jl rows to owned History
// display records. The firmware adapts these records to the borrowed lib/ui
// HistoryEntry span after publishing them under its state mutex.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "os_client.h"

namespace osp {

enum class HistoryKind {
  ProgramRun,
  ManualRun,
  RunOnce,
  RainDelay,
  Sensor1,
  Sensor2,
};

struct HistoryRecord {
  std::string name;
  uint32_t duration_s = 0;
  std::string when;
  HistoryKind kind = HistoryKind::ProgramRun;
  std::string tag;
};

std::string format_history_timestamp(uint32_t end_epoch,
                                     uint32_t now_local_epoch);

std::vector<HistoryRecord> build_history_records(
    const std::vector<LogEntry>& logs,
    const std::vector<std::string>& station_names,
    const JpData& programs,
    uint32_t now_local_epoch,
    std::size_t max_records);

}  // namespace osp
