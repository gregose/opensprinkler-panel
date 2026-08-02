// Host LVGL simulator for the OpenSprinkler panel (issue #125).
//
// Renders the REAL firmware screen code (lib/ui) on the host to 480x320 PNGs,
// using the SAME lv_conf.h + Montserrat fonts as the ESP32 firmware, so there
// is no HTML->LVGL translation drift and the sim IS the firmware rendering for
// every LVGL screen/state. Headless by default (no display needed, CI safe);
// pass --window to also open an SDL preview of one state.
//
// Pipeline:
//   1. lv_init + a host display backed by a full 480x320 RGB565 framebuffer
//      whose flush_cb copies rendered pixels into that framebuffer.
//   2. For each named state, build the composite screen via
//      osp::ui::build_panel_screen()/build_station_grid(), apply the fixture
//      view-model inputs, and drive it with osp::ui::update_panel_screen().
//   3. lv_refr_now() -> encode the framebuffer to sim/out/<state>.png (lodepng).
//   4. For states with a committed bench reference, diff the full frame and
//      print a fidelity report (MAE / %-within-tol), same bands as #124.
//
// Usage:
//   program                      render every state to sim/out/
//   program --state run-manual   render one state (to sim/out/run-manual.png)
//   program --out path.png       override the output path (single-state only)
//   program --outdir dir         override the output directory (all states)
//   program --window [--state s] open an SDL preview of one state
//
// lodepng (sim/lodepng.*) is a host-only, public-domain single-file PNG codec.
// It is intentionally NOT under lib/ (the repo's "no vendoring into lib/" rule
// is about firmware libraries; this is a build-time tool for the sim only).

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <lvgl.h>

#include "panel_screen.h"
#include "ui_theme.h"

#include "fixtures.h"
#include "lodepng.h"

namespace {

constexpr int kW = SCREEN_W;  // 480
constexpr int kH = SCREEN_H;  // 320

// Full-screen RGB565 framebuffer written by the flush callback.
uint16_t g_fb[kW * kH];

uint32_t tick_ms() {
    using namespace std::chrono;
    static const auto t0 = steady_clock::now();
    return static_cast<uint32_t>(
        duration_cast<milliseconds>(steady_clock::now() - t0).count());
}

// Copy the rendered RGB565 pixels for `area` into g_fb, then release the buffer.
void flush_cb(lv_display_t* disp, const lv_area_t* area, uint8_t* px_map) {
    const int32_t x1 = area->x1, y1 = area->y1, x2 = area->x2, y2 = area->y2;
    const uint16_t* src = reinterpret_cast<const uint16_t*>(px_map);
    for (int32_t y = y1; y <= y2; ++y) {
        for (int32_t x = x1; x <= x2; ++x) {
            if (x >= 0 && x < kW && y >= 0 && y < kH) {
                g_fb[y * kW + x] = *src;
            }
            ++src;
        }
    }
    lv_display_flush_ready(disp);
}

inline void rgb565_to_rgb888(uint16_t p, uint8_t& r, uint8_t& g, uint8_t& b) {
    const uint8_t r5 = (p >> 11) & 0x1f;
    const uint8_t g6 = (p >> 5) & 0x3f;
    const uint8_t b5 = p & 0x1f;
    r = static_cast<uint8_t>((r5 * 255 + 15) / 31);
    g = static_cast<uint8_t>((g6 * 255 + 31) / 63);
    b = static_cast<uint8_t>((b5 * 255 + 15) / 31);
}

// Encode the whole framebuffer (RGB565 -> RGB888) to a PNG file.
bool write_png(const std::string& path) {
    std::vector<unsigned char> rgb(static_cast<size_t>(kW) * kH * 3);
    for (int i = 0; i < kW * kH; ++i) {
        rgb565_to_rgb888(g_fb[i], rgb[i * 3 + 0], rgb[i * 3 + 1], rgb[i * 3 + 2]);
    }
    unsigned err = lodepng::encode(path, rgb, kW, kH, LCT_RGB, 8);
    if (err) {
        std::fprintf(stderr, "lodepng encode error %u: %s\n", err,
                     lodepng_error_text(err));
        return false;
    }
    return true;
}

// Build the composite panel screen and drive it into `fx`'s state. The screen
// is rebuilt from scratch each call (the active screen is cleaned first) so
// successive states don't accumulate widgets. Event callbacks are left null
// (the sim is non-interactive).
void render_scene(const osp::sim::Fixture& fx) {
    lv_obj_t* scr = lv_screen_active();
    lv_obj_clean(scr);

    osp::ui::PanelScreen ps =
        osp::ui::build_panel_screen(scr, osp::ui::Callbacks{});
    // Local non-const copy: update_panel_screen takes StationModel& (read-only).
    osp::StationModel model = fx.model;
    osp::ui::build_station_grid(ps, model);
    osp::ui::HistoryView history;
    history.entries = fx.history_entries.empty() ? nullptr
                                                 : fx.history_entries.data();
    history.count   = static_cast<int>(fx.history_entries.size());
    history.page    = fx.view.hist_list_page;
    osp::ui::update_panel_screen(ps, fx.view, model, fx.programs, history,
                                 fx.host);
}

// Compare the rendered full frame to the reference PNG and print stats.
// Returns true if the reference was found and diffed.
bool diff_full(const std::string& ref_path, const std::string& diff_out) {
    std::vector<unsigned char> ref;
    unsigned rw = 0, rh = 0;
    unsigned err = lodepng::decode(ref, rw, rh, ref_path, LCT_RGB, 8);
    if (err) {
        std::fprintf(stderr, "  (no reference %s: err %u)\n",
                     ref_path.c_str(), err);
        return false;
    }
    const int cmp_w = std::min<int>(kW, static_cast<int>(rw));
    const int cmp_h = std::min<int>(kH, static_cast<int>(rh));

    long long sum_abs = 0;
    long long n = 0;
    int within8 = 0, within16 = 0, within32 = 0;
    std::vector<unsigned char> diff_img(static_cast<size_t>(cmp_w) * cmp_h * 3);

    for (int y = 0; y < cmp_h; ++y) {
        for (int x = 0; x < cmp_w; ++x) {
            uint8_t rr, rg, rb;
            rgb565_to_rgb888(g_fb[y * kW + x], rr, rg, rb);
            const size_t ri = (static_cast<size_t>(y) * rw + x) * 3;
            const int dr = std::abs(rr - ref[ri + 0]);
            const int dg = std::abs(rg - ref[ri + 1]);
            const int db = std::abs(rb - ref[ri + 2]);
            const int worst = std::max(dr, std::max(dg, db));
            sum_abs += dr + dg + db;
            n += 3;
            if (worst <= 8) ++within8;
            if (worst <= 16) ++within16;
            if (worst <= 32) ++within32;
            const size_t di = (static_cast<size_t>(y) * cmp_w + x) * 3;
            diff_img[di + 0] = static_cast<unsigned char>(dr);
            diff_img[di + 1] = static_cast<unsigned char>(dg);
            diff_img[di + 2] = static_cast<unsigned char>(db);
        }
    }
    const int px = cmp_w * cmp_h;
    const double mae = n ? static_cast<double>(sum_abs) / n : 0.0;
    std::printf("  fidelity vs %s: MAE %.2f/255, within 8/16/32 = %.1f%% / %.1f%% / %.1f%%\n",
                ref_path.c_str(), mae,
                100.0 * within8 / px, 100.0 * within16 / px,
                100.0 * within32 / px);

    if (!diff_out.empty()) {
        lodepng::encode(diff_out, diff_img, cmp_w, cmp_h, LCT_RGB, 8);
    }
    return true;
}

const std::string kRefDir = "site/assets/img/screenshots/";

// Render one named state to `out_png` (+ a diff PNG when a bench ref exists).
bool render_one(const std::string& state, const std::string& out_png,
                const std::string& diff_png) {
    osp::sim::Fixture fx = osp::sim::make_fixture(state);
    std::memset(g_fb, 0, sizeof(g_fb));
    render_scene(fx);
    lv_refr_now(nullptr);
    if (!write_png(out_png)) return false;
    std::printf("rendered %-20s -> %s\n", state.c_str(), out_png.c_str());
    if (!fx.ref.empty()) {
        diff_full(kRefDir + fx.ref, diff_png);
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    bool want_window = false;
    std::string one_state;   // empty => render all
    std::string out_png;     // explicit single-state output override
    std::string outdir = "sim/out";
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--window") want_window = true;
        else if (a == "--state" && i + 1 < argc) one_state = argv[++i];
        else if (a == "--out" && i + 1 < argc) out_png = argv[++i];
        else if (a == "--outdir" && i + 1 < argc) outdir = argv[++i];
    }

    lv_init();
    lv_tick_set_cb(tick_ms);

    // Headless display backed by the full-screen RGB565 framebuffer.
    static uint8_t draw_buf[kW * kH * 2];
    lv_display_t* disp = lv_display_create(kW, kH);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(disp, draw_buf, nullptr, sizeof(draw_buf),
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(disp, flush_cb);

#if defined(LV_USE_SDL) && LV_USE_SDL
    if (want_window) {
        const std::string state = one_state.empty() ? "idle-connected" : one_state;
        lv_display_t* win = lv_sdl_window_create(kW, kH);
        (void)win;
        std::printf("SDL window: rendering '%s'; close it to exit.\n",
                    state.c_str());
        render_scene(osp::sim::make_fixture(state));
        while (true) {
            lv_timer_handler();
            lv_delay_ms(16);
        }
    }
#else
    (void)want_window;
#endif

    int failures = 0;
    if (!one_state.empty()) {
        const std::string out = out_png.empty()
            ? outdir + "/" + one_state + ".png" : out_png;
        const std::string diff = outdir + "/" + one_state + "-diff.png";
        if (!render_one(one_state, out, diff)) ++failures;
    } else {
        for (const auto& s : osp::sim::all_states()) {
            const std::string out = outdir + "/" + s + ".png";
            const std::string diff = outdir + "/" + s + "-diff.png";
            if (!render_one(s, out, diff)) ++failures;
        }
    }
    return failures ? 1 : 0;
}
