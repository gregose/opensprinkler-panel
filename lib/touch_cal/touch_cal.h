// Hardware-independent XPT2046 resistive touch calibration.
//
// Everything here is pure C++ (no Arduino, no network) so it builds and runs
// under the PlatformIO `native` test environment.
//
// The calibration is a full affine transform (6 coefficients) that maps raw
// ADC coordinates to screen pixels:
//
//   px = a*rx + b*ry + c
//   py = d*rx + e*ry + f
//
// Affine (not per-axis linear) so it tolerates the axis swap, rotation, and
// skew these resistive panels exhibit in landscape orientation.
//
// Byte blob layout (25 bytes, suitable for NVS "touch_cal" key):
//   [0]      version tag (0x01)
//   [1..4]   a — IEEE-754 single, little-endian
//   [5..8]   b
//   [9..12]  c
//   [13..16] d
//   [17..20] e
//   [21..24] f
#pragma once

#include <cstdint>
#include <vector>

namespace osp {

// A single calibration sample: raw ADC coordinate pair and the known screen
// pixel it corresponds to.
struct CalPoint {
    int raw_x;
    int raw_y;
    int screen_x;
    int screen_y;
};

// Affine calibration coefficients.
//   px = a*rx + b*ry + c
//   py = d*rx + e*ry + f
struct TouchCalibration {
    float a = 1.0f, b = 0.0f, c = 0.0f;  // x-axis transform
    float d = 0.0f, e = 1.0f, f = 0.0f;  // y-axis transform
};

// Output of apply().
struct ScreenPoint {
    int x;
    int y;
};

// Compute a least-squares affine fit from ≥ 3 non-collinear CalPoints.
// Returns false (and leaves `out` unchanged) if:
//   - points.size() < 3, or
//   - the raw-coordinate matrix is degenerate (collinear / duplicate points).
bool fit(const std::vector<CalPoint>& points, TouchCalibration& out);

// Apply the calibration to a raw ADC coordinate and clamp the result to
// [0, screen_w) × [0, screen_h).  Screen dimensions are passed in so the
// transform itself remains dimension-agnostic.
ScreenPoint apply(const TouchCalibration& cal,
                  int raw_x, int raw_y,
                  int screen_w, int screen_h);

// Blob size and version tag for NVS storage.
static constexpr std::size_t kTouchCalBlobSize = 25;
static constexpr uint8_t     kTouchCalVersion  = 0x01;

// Serialize `cal` to a 25-byte blob (versioned, endian-safe IEEE-754 LE).
std::vector<uint8_t> to_bytes(const TouchCalibration& cal);

// Deserialize a blob produced by to_bytes().
// Returns false if the blob is too short or the version tag does not match.
bool from_bytes(const std::vector<uint8_t>& blob, TouchCalibration& out);

}  // namespace osp
