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
};

// The full History view model: a borrowed span of entries plus the current
// page. Total page count is derived from `count` and the screen's rows-per-page
// in update_panel_screen(), mirroring how the programs list pages from JpData.
struct HistoryView {
    const HistoryEntry* entries = nullptr;
    int                 count   = 0;
    int                 page    = 0;
};

}  // namespace ui
}  // namespace osp
