// Sim fixtures: view-model inputs that drive each on-glass screen/state.
//
// The host sim renders the REAL firmware screen code (lib/ui) headlessly. Each
// named state assembles the same pure inputs the firmware feeds into
// osp::ui::update_panel_screen() (PanelView + StationModel + JpData programs +
// HostStatus), so the sim exercises every screen without any Arduino/network.
// No firmware symbols here: this is portable host-only C++.
#pragma once

#include <string>
#include <vector>

#include "os_client.h"       // osp::JpData
#include "panel_screen.h"    // osp::ui::HostStatus
#include "panel_state.h"     // osp::PanelView
#include "station_model.h"   // osp::StationModel

namespace osp {
namespace sim {

// One fully-assembled scene. `model` is non-const because update_panel_screen()
// takes a StationModel& (it only reads); `ref` is the basename of a committed
// bench screenshot to diff against, or empty for render-only states.
struct Fixture {
    std::string     name;
    PanelView       view;
    StationModel    model;
    JpData          programs;
    ui::HostStatus  host;
    std::string     ref;  // "" when no committed bench reference exists

    // #127: render-only history rows for the History overlay. Owned here so the
    // borrowed const char* fields in each entry (string literals) stay valid;
    // sim_main builds a ui::HistoryView spanning this vector.
    std::vector<ui::HistoryEntry> history_entries;
};

// Build the fixture for a named state (see kAllStates for the list). Unknown
// names fall back to the idle-connected scene.
Fixture make_fixture(const std::string& state);

// Every renderable state, in a stable order (used by the render-all driver).
const std::vector<std::string>& all_states();

}  // namespace sim
}  // namespace osp
