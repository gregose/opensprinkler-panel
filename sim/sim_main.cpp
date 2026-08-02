// Host LVGL simulator for the OpenSprinkler panel top bar (issue #123 spike).
//
// Renders the REAL firmware top-bar code (lib/ui) on the host to a 480x320 PNG,
// using the SAME lv_conf.h + Montserrat fonts as the ESP32 firmware, so there is
// no HTML->LVGL translation drift. Headless by default (no display needed, CI
// safe); pass --window to also open an SDL preview window.
//
// Pipeline:
//   1. lv_init + a host display backed by a full 480x320 RGB565 framebuffer with
//      a flush_cb that copies rendered pixels into that framebuffer.
//   2. Build the top bar via osp::ui::build_top_bar() and drive it into the
//      representative "idle connected" state (teal droplet + IP name + P/C
//      meters + battery + 3px teal rule).
//   3. lv_refr_now() -> encode the framebuffer to sim/out/top-bar.png (lodepng).
//   4. Diff the rendered top band against the top band of the committed bench
//      reference (site/assets/img/screenshots/home-connected.png) and print a
//      fidelity report.
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

#include "battery_monitor.h"  // osp::BatteryTier
#include "top_bar.h"
#include "ui_theme.h"

#include "lodepng.h"

namespace {

constexpr int kW = SCREEN_W;  // 480
constexpr int kH = SCREEN_H;  // 320
constexpr int kTopBand = TOP_H + 3;  // 29 px: 26 bar + 3 accent rule

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

// Build the top bar and drive it into the representative idle-connected state.
void build_scene() {
    lv_obj_t* scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, hex_color(CLR_BG), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    osp::ui::TopBar tb = osp::ui::build_top_bar(scr);

    // Idle + connected: teal droplet (default), an IP-style identity, no status
    // pill, strong panel + controller signal, a healthy battery, teal rule.
    lv_label_set_text(tb.lbl_name, "192.168.1.50");
    lv_label_set_text(tb.lbl_status, "");
    osp::ui::update_sig_meter(tb.sig_panel, /*quality=*/4, /*connected=*/true);
    osp::ui::update_sig_meter(tb.sig_ctrl, /*quality=*/4, /*connected=*/true);
    osp::ui::update_batt_glyph(tb.batt, /*percent=*/82,
                               osp::BatteryTier::Healthy);
}

// Compare the rendered top band to the reference PNG's top band and print stats.
// Returns true if the reference was found and diffed.
bool diff_top_band(const std::string& ref_path, const std::string& diff_out) {
    std::vector<unsigned char> ref;
    unsigned rw = 0, rh = 0;
    unsigned err = lodepng::decode(ref, rw, rh, ref_path, LCT_RGB, 8);
    if (err) {
        std::fprintf(stderr, "Could not decode reference %s (err %u: %s)\n",
                     ref_path.c_str(), err, lodepng_error_text(err));
        return false;
    }
    const int band_h = kTopBand;
    const int cmp_w = std::min<int>(kW, static_cast<int>(rw));
    const int cmp_h = std::min<int>(band_h, static_cast<int>(rh));

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
    std::printf("\n=== Top-bar fidelity vs %s ===\n", ref_path.c_str());
    std::printf("compared band: %dx%d px (%d px)\n", cmp_w, cmp_h, px);
    std::printf("mean abs error: %.2f / 255 per channel\n", mae);
    std::printf("pixels within  8/255: %.1f%%\n", 100.0 * within8 / px);
    std::printf("pixels within 16/255: %.1f%%\n", 100.0 * within16 / px);
    std::printf("pixels within 32/255: %.1f%%\n", 100.0 * within32 / px);

    if (!diff_out.empty()) {
        unsigned e = lodepng::encode(diff_out, diff_img, cmp_w, cmp_h, LCT_RGB, 8);
        if (!e) std::printf("diff image written: %s\n", diff_out.c_str());
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    bool want_window = false;
    std::string out_png = "sim/out/top-bar.png";
    std::string ref_png = "site/assets/img/screenshots/home-connected.png";
    std::string diff_png = "sim/out/top-bar-diff.png";
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--window") want_window = true;
        else if (a == "--out" && i + 1 < argc) out_png = argv[++i];
        else if (a == "--ref" && i + 1 < argc) ref_png = argv[++i];
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

    std::memset(g_fb, 0, sizeof(g_fb));
    build_scene();
    lv_refr_now(disp);

    if (!write_png(out_png)) return 1;
    std::printf("rendered top bar: %s (%dx%d)\n", out_png.c_str(), kW, kH);

    diff_top_band(ref_png, diff_png);

#if defined(LV_USE_SDL) && LV_USE_SDL
    if (want_window) {
        // Optional local preview: mirror the scene into an SDL window and spin.
        lv_display_t* win = lv_sdl_window_create(kW, kH);
        (void)win;
        std::printf("SDL window open; close it to exit.\n");
        // Rebuild the scene on the SDL display's active screen.
        build_scene();
        while (true) {
            lv_timer_handler();
            lv_delay_ms(16);
        }
    }
#else
    (void)want_window;
#endif
    return 0;
}
