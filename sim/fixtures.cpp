// Sim fixtures implementation. See fixtures.h.
#include "fixtures.h"

#include <array>

#include "battery_monitor.h"  // osp::BatteryTier
#include "program_model.h"    // osp::Program, osp::ProgramRunState

namespace osp {
namespace sim {
namespace {

// Shared demo station set (8 named zones, all enabled, no master). Mirrors a
// typical residential controller; used by every scene so the grid and run
// screens have stable, human-readable labels.
StationModel demo_stations() {
    StationModel m;
    const std::vector<std::string> names = {
        "Front Lawn", "Back Lawn", "Garden Drip", "Side Yard",
        "Rose Bed",   "Veggie Beds", "Patio Pots", "Parkway"};
    m.load(names, /*stn_dis=*/{0}, /*mas=*/0, /*mas2=*/0);
    return m;
}

// Shared demo program set (5 programs) for the programs list + run header.
JpData demo_programs() {
    JpData jp;
    jp.nprogs = 5;
    const char* pnames[] = {"Morning Cycle", "Evening Soak", "Drip Daily",
                            "Weekend Deep", "Parkway Only"};
    const bool penabled[] = {true, true, false, true, false};
    for (int i = 0; i < 5; ++i) {
        Program p;
        p.enabled = penabled[i];
        p.type = ProgramType::Weekly;
        p.starttime_type_fixed = true;
        p.days0 = 0x7f;  // every day
        p.starttimes = {static_cast<int16_t>(360 + i * 30), -1, -1, -1};
        p.durations = {300, 300, 600, 300};
        p.name = pnames[i];
        jp.programs.push_back(p);
    }
    return jp;
}

// A healthy, connected top-bar HostStatus (panel Wi-Fi + battery gauge).
ui::HostStatus healthy_host() {
    ui::HostStatus h;
    h.panel_rssi_bars = 4;
    h.panel_connected = true;
    h.battery_percent = 82;
    h.battery_tier    = BatteryTier::Healthy;
    h.host_name       = "opensprinkler.local";
    return h;
}

// Baseline idle view: stations loaded, connected, teal rule.
PanelView idle_view() {
    PanelView v;
    v.phase = Phase::Idle;
    v.station_list_loaded = true;
    v.link = LinkState::Connected;
    v.controller_identity = "opensprinkler.local";
    v.enabled = true;
    v.run_time_s = 300;
    v.auto_advance = true;
    v.ctrl_rssi = -58;
    v.ctrl_devt = 1700000000L;  // stable epoch for next-run computation
    v.sunrise_min = 6 * 60;
    v.sunset_min  = 20 * 60;
    v.has_current = true;
    v.current_ma = 230;
    return v;
}

// Program-run queue: 5 stations, currently on #3, two done above.
ProgramRunState demo_prog_run() {
    ProgramRunState pr;
    pr.run_class = RunClass::ProgramRun;
    pr.program_index = 0;  // "Morning Cycle"
    pr.current_sid = 2;    // Garden Drip
    pr.current_station_number = 3;
    pr.station_count = 5;
    pr.total_remaining_seconds = 11 * 60 + 40;
    const int sids[]  = {0, 1, 2, 3, 5};
    const int totals[] = {300, 300, 600, 300, 300};
    for (int i = 0; i < 5; ++i) {
        ProgramQueueEntry e;
        e.sid = sids[i];
        e.total_seconds = totals[i];
        e.started = (i <= 2);
        e.done    = (i < 2);
        e.remaining_seconds = (i == 2) ? 214 : (i < 2 ? 0 : totals[i]);
        pr.queue.push_back(e);
    }
    return pr;
}

}  // namespace

Fixture make_fixture(const std::string& state) {
    Fixture f;
    f.name = state;
    f.model = demo_stations();
    f.programs = demo_programs();
    f.host = healthy_host();
    f.view = idle_view();

    if (state == "idle-connected") {
        f.ref = "home-connected.png";
    } else if (state == "idle-loading") {
        f.view.station_list_loaded = false;
        f.view.has_current = false;
    } else if (state == "idle-syncing") {
        f.view.show_syncing = true;
    } else if (state == "idle-reconnecting") {
        f.view.link = LinkState::Reconnecting;
    } else if (state == "idle-offline") {
        f.view.link = LinkState::Offline;
        f.host.panel_connected = true;  // panel online, controller not
    } else if (state == "idle-auth") {
        f.view.link = LinkState::AuthError;
    } else if (state == "run-manual") {
        f.view.phase = Phase::Running;
        f.view.running_sid = 0;   // Front Lawn
        f.view.run_time_s = 600;
        f.view.countdown_s = 9 * 60 + 12;
        f.view.run_initiated_by_panel = true;
        f.ref = "manual-run.png";
    } else if (state == "run-manual-paused") {
        f.view.phase = Phase::Running;
        f.view.running_sid = 0;
        f.view.run_time_s = 600;
        f.view.countdown_s = 9 * 60 + 12;
        f.view.paused = true;
        f.view.pause_remaining_s = 4 * 60 + 30;
        f.ref = "manual-paused.png";
    } else if (state == "run-program") {
        f.view.phase = Phase::ProgramRunning;
        f.view.prog_run = demo_prog_run();
        f.view.countdown_s = 3 * 60 + 34;
        f.ref = "program-running.png";
    } else if (state == "run-program-paused") {
        f.view.phase = Phase::ProgramRunning;
        f.view.prog_run = demo_prog_run();
        f.view.countdown_s = 3 * 60 + 34;
        f.view.paused = true;
        f.view.pause_remaining_s = 4 * 60 + 30;
        f.ref = "program-paused.png";
    } else if (state == "programs-list") {
        f.view.showing_programs_list = true;
        f.view.prog_list_page = 0;
        f.ref = "programs-list.png";
    } else if (state == "programs-list-paged") {
        f.view.showing_programs_list = true;
        f.view.prog_list_page = 1;
    } else if (state == "history-list" || state == "history-list-paged") {
        // Realistic mixed log across named yard zones: program runs, one manual
        // run, one run-once. 11 rows -> 2 pages (7 + 4) so the pager engages.
        f.view.showing_history = true;
        f.view.hist_list_page = (state == "history-list-paged") ? 1 : 0;
        using HE = ui::HistoryEntry;
        f.history_entries = {
            {"Front Lawn",  600, "Today 6:32a", HE::ProgramRun},
            {"Back Lawn",   600, "Today 6:22a", HE::ProgramRun},
            {"Rose Bed",    300, "Today 6:12a", HE::ProgramRun},
            {"Garden Drip", 900, "Today 6:00a", HE::ProgramRun},
            {"Backyard Vegetable Garden Beds", 120, "Today 5:45a", HE::ManualRun},
            {"Veggie Beds", 480, "Today 5:30a", HE::RunOnce},
            {"Side Yard",   300, "Tue 8:15p",   HE::ProgramRun},
            {"Front Lawn",  600, "Tue 6:32a",   HE::ProgramRun},
            {"Back Lawn",   600, "Tue 6:22a",   HE::ProgramRun},
            {"Parkway",     240, "Mon 7:10p",   HE::ManualRun},
            {"Garden Drip", 900, "Jul 28",      HE::ProgramRun},
        };
    } else if (state == "history-mixed-events") {
        // A page that mixes runs with a rain-delay + sensor events so their
        // icons and dimming are visible alongside normal runs.
        f.view.showing_history = true;
        f.view.hist_list_page = 0;
        using HE = ui::HistoryEntry;
        f.history_entries = {
            {"Front Lawn",   600,   "Today 6:32a", HE::ProgramRun},
            {"Rain delay",   86400, "Today 5:00a", HE::RainDelay},
            {"Back Lawn",    600,   "Mon 6:22a",   HE::ProgramRun},
            {"Rain sensor",  0,     "Mon 4:15a",   HE::Sensor1},
            {"Garden Drip",  900,   "Sun 6:00a",   HE::ProgramRun},
            {"Rose Bed",     300,   "Sun 5:48a",   HE::ProgramRun},
            {"Soil sensor",  0,     "Sat 9:30p",   HE::Sensor2},
        };
    } else if (state == "history-empty") {
        // No log yet: centred empty-state message, Back + top bar still shown.
        f.view.showing_history = true;
        f.view.hist_list_page = 0;
        f.history_entries = {};
    } else if (state == "sleep") {
        f.view.sleeping = true;
    }
    return f;
}

const std::vector<std::string>& all_states() {
    static const std::vector<std::string> kAll = {
        "idle-connected", "idle-loading", "idle-syncing", "idle-reconnecting",
        "idle-offline", "idle-auth", "run-manual", "run-manual-paused",
        "run-program", "run-program-paused", "programs-list",
        "programs-list-paged", "history-list", "history-list-paged",
        "history-mixed-events", "history-empty", "sleep"};
    return kAll;
}

}  // namespace sim
}  // namespace osp
