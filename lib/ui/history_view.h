// Portable view model for the on-panel History screen (lib/ui).
//
// LVGL-agnostic, pure C++ (no Arduino, no TFT_eSPI, no network) so the firmware
// and the host `sim` link the same rendering code. This is the render-only
// contract for the History overlay: a flat list of already-formatted log rows
// plus the current page. PR1 supplies these directly from sim fixtures; PR2
// will populate them from the controller's /jl log endpoint.
#pragma once

#include <cstdint>

namespace osp {
namespace ui {

// One history log row, fully precomputed for rendering. `name` and `when` are
// borrowed C strings owned by the caller (fixture storage or the /jl cache);
// they must outlive the update_panel_screen() call that consumes them.
struct HistoryEntry {
    enum Kind {
        ProgramRun,  // scheduled program station run (droplet)
        ManualRun,   // manual station run, pid 99 (droplet, distinct tint)
        RunOnce,     // run-once program, pid 254 (loop)
        RainDelay,   // rain-delay event (warning, amber)
        Sensor1,     // sensor 1 event (bell)
        Sensor2,     // sensor 2 event (bell)
    };

    const char* name  = "";   // station or program name
    uint32_t    dur_s = 0;    // precomputed run duration, seconds
    const char* when  = "";   // precomputed short timestamp, e.g. "Today 6:32a"
    Kind        kind  = ProgramRun;
    // Optional context label shown after the name (muted). For a program run
    // this is the program name (e.g. "Morning"); manual runs default to
    // "Manual" when null. Events (rain delay, sensors) leave it null.
    const char* tag   = nullptr;
};

// The full History view model: a borrowed span of entries plus the current
// page. Total page count is derived from `count` and the screen's rows-per-page
// in update_panel_screen(), mirroring how the programs list pages from JpData.
struct HistoryView {
    const HistoryEntry* entries = nullptr;
    int                 count   = 0;
    int                 page    = 0;
};

// In-memory cap on retained /jl records. The classic ESP32 has no PSRAM, so PR2
// keeps only the most recent HISTORY_MAX_RECORDS log entries (newest-first) and
// drops the rest. At MAX_HIST_ROWS per page this is the worst-case page count
// the numeric pager must handle (120 / 10 = 12 pages). Tunable.
static constexpr int HISTORY_MAX_RECORDS = 120;

}  // namespace ui
}  // namespace osp
