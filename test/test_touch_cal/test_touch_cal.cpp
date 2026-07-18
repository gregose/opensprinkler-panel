// Native (host) unit tests for the hardware-independent touch calibration.
// Runs under `pio test -e native` — no board, network, or LVGL required.
#include <unity.h>

#include <cstring>
#include <vector>

#include "touch_cal.h"

using namespace osp;

void setUp() {}
void tearDown() {}

static const int W = 480;
static const int H = 320;

// ---------------------------------------------------------------------------
// fit — identity / simple linear map
// ---------------------------------------------------------------------------

// Fit a simple 1:1 (identity) map: raw == screen.
void test_fit_identity_map() {
    std::vector<CalPoint> pts = {
        {  0,   0,   0,   0},
        {100,   0, 100,   0},
        {  0, 100,   0, 100},
    };
    TouchCalibration cal;
    TEST_ASSERT_TRUE(fit(pts, cal));

    // Coefficients should be: a=1 b=0 c=0  d=0 e=1 f=0
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 1.0f, cal.a);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.0f, cal.b);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.0f, cal.c);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.0f, cal.d);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 1.0f, cal.e);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.0f, cal.f);

    // Applying back should give exact results at an interior point.
    ScreenPoint p = apply(cal, 50, 75, W, H);
    TEST_ASSERT_INT_WITHIN(1, 50, p.x);
    TEST_ASSERT_INT_WITHIN(1, 75, p.y);
}

// Fit with a non-trivial linear scaling (raw 0..4095 -> screen 0..479/319).
void test_fit_linear_scaling() {
    // 3 reference points, exact linear scaling.
    std::vector<CalPoint> pts = {
        {   0,    0,   0,   0},
        {4095,    0, 479,   0},
        {   0, 4095,   0, 319},
    };
    TouchCalibration cal;
    TEST_ASSERT_TRUE(fit(pts, cal));

    // Apply the reference points and check within ±1 px (float rounding).
    ScreenPoint p0 = apply(cal,    0,    0, W, H);
    TEST_ASSERT_INT_WITHIN(1, 0,   p0.x);
    TEST_ASSERT_INT_WITHIN(1, 0,   p0.y);

    ScreenPoint p1 = apply(cal, 4095,    0, W, H);
    TEST_ASSERT_INT_WITHIN(1, 478, p1.x);  // 479 clamps to 479, float may give 478
    TEST_ASSERT_INT_WITHIN(1, 0,   p1.y);

    ScreenPoint p2 = apply(cal,    0, 4095, W, H);
    TEST_ASSERT_INT_WITHIN(1, 0,   p2.x);
    TEST_ASSERT_INT_WITHIN(1, 318, p2.y);  // 319 within ±1
}

// Fit with more than 3 points (overdetermined — tests least-squares path).
void test_fit_overdetermined() {
    // 4 points on the exact same linear map: screen_x = rx/10, screen_y = ry/10
    std::vector<CalPoint> pts = {
        {  0,   0,  0,  0},
        {100,   0, 10,  0},
        {  0, 100,  0, 10},
        {100, 100, 10, 10},
    };
    TouchCalibration cal;
    TEST_ASSERT_TRUE(fit(pts, cal));

    ScreenPoint p = apply(cal, 50, 50, W, H);
    TEST_ASSERT_INT_WITHIN(1, 5, p.x);
    TEST_ASSERT_INT_WITHIN(1, 5, p.y);
}

// ---------------------------------------------------------------------------
// fit — rotated / axis-swapped panel
// ---------------------------------------------------------------------------

// Simulate an axis-swapped panel: raw_x drives screen_y, raw_y drives screen_x.
void test_fit_axis_swapped() {
    // px = 0*rx + 0.75*ry + 0
    // py = 0.50*rx + 0*ry + 0
    std::vector<CalPoint> pts = {
        {   0,   0,   0,   0},
        { 100,   0,   0,  50},
        {   0, 100,  75,   0},
    };
    TouchCalibration cal;
    TEST_ASSERT_TRUE(fit(pts, cal));

    // Reference points must round-trip within ±1 px.
    ScreenPoint p0 = apply(cal,   0,   0, W, H);
    TEST_ASSERT_INT_WITHIN(1, 0,  p0.x);
    TEST_ASSERT_INT_WITHIN(1, 0,  p0.y);

    ScreenPoint p1 = apply(cal, 100,   0, W, H);
    TEST_ASSERT_INT_WITHIN(1, 0,  p1.x);
    TEST_ASSERT_INT_WITHIN(1, 50, p1.y);

    ScreenPoint p2 = apply(cal,   0, 100, W, H);
    TEST_ASSERT_INT_WITHIN(1, 75, p2.x);
    TEST_ASSERT_INT_WITHIN(1, 0,  p2.y);
}

// ---------------------------------------------------------------------------
// apply — clamping
// ---------------------------------------------------------------------------

void test_apply_clamps_negative() {
    // Identity calibration; raw values that map to negative pixels.
    TouchCalibration cal;  // default: a=1 b=0 c=0  d=0 e=1 f=0
    cal.a = 1.0f; cal.b = 0.0f; cal.c = 0.0f;
    cal.d = 0.0f; cal.e = 1.0f; cal.f = 0.0f;

    ScreenPoint p = apply(cal, -50, -100, W, H);
    TEST_ASSERT_EQUAL_INT(0, p.x);
    TEST_ASSERT_EQUAL_INT(0, p.y);
}

void test_apply_clamps_over_width() {
    // Identity calibration; raw x beyond screen width.
    TouchCalibration cal;
    cal.a = 1.0f; cal.b = 0.0f; cal.c = 0.0f;
    cal.d = 0.0f; cal.e = 1.0f; cal.f = 0.0f;

    ScreenPoint p = apply(cal, 600, 100, W, H);
    TEST_ASSERT_EQUAL_INT(W - 1, p.x);
    TEST_ASSERT_EQUAL_INT(100,   p.y);
}

void test_apply_clamps_over_height() {
    TouchCalibration cal;
    cal.a = 1.0f; cal.b = 0.0f; cal.c = 0.0f;
    cal.d = 0.0f; cal.e = 1.0f; cal.f = 0.0f;

    ScreenPoint p = apply(cal, 100, 400, W, H);
    TEST_ASSERT_EQUAL_INT(100,   p.x);
    TEST_ASSERT_EQUAL_INT(H - 1, p.y);
}

void test_apply_clamps_exact_boundary() {
    // Computed pixel exactly equal to screen dimension must clamp to dim-1.
    TouchCalibration cal;
    cal.a = 1.0f; cal.b = 0.0f; cal.c = 0.0f;
    cal.d = 0.0f; cal.e = 1.0f; cal.f = 0.0f;

    ScreenPoint px_exact = apply(cal, W, 0, W, H);
    TEST_ASSERT_EQUAL_INT(W - 1, px_exact.x);

    ScreenPoint py_exact = apply(cal, 0, H, W, H);
    TEST_ASSERT_EQUAL_INT(H - 1, py_exact.y);
}

// ---------------------------------------------------------------------------
// fit — degenerate input rejection
// ---------------------------------------------------------------------------

void test_fit_rejects_zero_points() {
    TouchCalibration cal;
    TEST_ASSERT_FALSE(fit({}, cal));
}

void test_fit_rejects_one_point() {
    TouchCalibration cal;
    TEST_ASSERT_FALSE(fit({{100, 200, 10, 20}}, cal));
}

void test_fit_rejects_two_points() {
    TouchCalibration cal;
    TEST_ASSERT_FALSE(fit({{0, 0, 0, 0}, {100, 100, 10, 10}}, cal));
}

void test_fit_rejects_collinear_points() {
    // Three raw points on the line ry = rx — raw matrix is rank-deficient.
    std::vector<CalPoint> pts = {
        {  0,   0,  0,  0},
        { 50,  50, 10, 20},
        {100, 100, 30, 40},
    };
    TouchCalibration cal;
    TEST_ASSERT_FALSE(fit(pts, cal));
}

void test_fit_rejects_duplicate_raw_points() {
    // Duplicate raw coordinates — also degenerate.
    std::vector<CalPoint> pts = {
        {50, 50,  0,  0},
        {50, 50, 10, 10},
        {50, 50, 20, 20},
    };
    TouchCalibration cal;
    TEST_ASSERT_FALSE(fit(pts, cal));
}

// ---------------------------------------------------------------------------
// to_bytes / from_bytes — round-trip
// ---------------------------------------------------------------------------

void test_blob_size() {
    TEST_ASSERT_EQUAL_INT(25, static_cast<int>(kTouchCalBlobSize));
}

void test_roundtrip_serialization() {
    std::vector<CalPoint> pts = {
        {   0,    0,   0,   0},
        {4000,    0, 460,   0},
        {   0, 3000,   0, 310},
    };
    TouchCalibration cal;
    TEST_ASSERT_TRUE(fit(pts, cal));

    const std::vector<uint8_t> blob = to_bytes(cal);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(kTouchCalBlobSize),
                          static_cast<int>(blob.size()));
    TEST_ASSERT_EQUAL_INT(kTouchCalVersion, blob[0]);

    TouchCalibration cal2;
    TEST_ASSERT_TRUE(from_bytes(blob, cal2));

    // The deserialized coefficients must be bit-for-bit identical.
    TEST_ASSERT_EQUAL_FLOAT(cal.a, cal2.a);
    TEST_ASSERT_EQUAL_FLOAT(cal.b, cal2.b);
    TEST_ASSERT_EQUAL_FLOAT(cal.c, cal2.c);
    TEST_ASSERT_EQUAL_FLOAT(cal.d, cal2.d);
    TEST_ASSERT_EQUAL_FLOAT(cal.e, cal2.e);
    TEST_ASSERT_EQUAL_FLOAT(cal.f, cal2.f);

    // Applying the two calibrations to the same raw point must give the same
    // result.
    ScreenPoint p1 = apply(cal,  200, 150, W, H);
    ScreenPoint p2 = apply(cal2, 200, 150, W, H);
    TEST_ASSERT_EQUAL_INT(p1.x, p2.x);
    TEST_ASSERT_EQUAL_INT(p1.y, p2.y);
}

void test_from_bytes_rejects_short_blob() {
    std::vector<uint8_t> short_blob(10, 0);
    TouchCalibration cal;
    TEST_ASSERT_FALSE(from_bytes(short_blob, cal));
}

void test_from_bytes_rejects_wrong_version() {
    // Build a syntactically correct blob but with a bad version byte.
    TouchCalibration src;
    src.a = 1.0f; src.b = 0.0f; src.c = 0.0f;
    src.d = 0.0f; src.e = 1.0f; src.f = 0.0f;
    std::vector<uint8_t> blob = to_bytes(src);
    blob[0] = 0xFF;  // corrupt version tag

    TouchCalibration out;
    TEST_ASSERT_FALSE(from_bytes(blob, out));
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(int, char**) {
    UNITY_BEGIN();

    RUN_TEST(test_fit_identity_map);
    RUN_TEST(test_fit_linear_scaling);
    RUN_TEST(test_fit_overdetermined);
    RUN_TEST(test_fit_axis_swapped);

    RUN_TEST(test_apply_clamps_negative);
    RUN_TEST(test_apply_clamps_over_width);
    RUN_TEST(test_apply_clamps_over_height);
    RUN_TEST(test_apply_clamps_exact_boundary);

    RUN_TEST(test_fit_rejects_zero_points);
    RUN_TEST(test_fit_rejects_one_point);
    RUN_TEST(test_fit_rejects_two_points);
    RUN_TEST(test_fit_rejects_collinear_points);
    RUN_TEST(test_fit_rejects_duplicate_raw_points);

    RUN_TEST(test_blob_size);
    RUN_TEST(test_roundtrip_serialization);
    RUN_TEST(test_from_bytes_rejects_short_blob);
    RUN_TEST(test_from_bytes_rejects_wrong_version);

    return UNITY_END();
}
