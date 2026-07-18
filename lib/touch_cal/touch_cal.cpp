#include "touch_cal.h"

#include <cmath>
#include <cstring>

namespace osp {

// ---------------------------------------------------------------------------
// Internal: solve a 3×3 linear system m·x = b using Gaussian elimination
// with partial pivoting.  Returns false if the matrix is (near-)singular.
// ---------------------------------------------------------------------------
static bool solve3(double m[3][3], const double b[3], double x[3]) {
    // Build augmented matrix [m | b].
    double aug[3][4];
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) aug[i][j] = m[i][j];
        aug[i][3] = b[i];
    }

    for (int col = 0; col < 3; ++col) {
        // Partial pivot: find row with largest absolute value in this column.
        int pivot = col;
        for (int row = col + 1; row < 3; ++row) {
            if (std::fabs(aug[row][col]) > std::fabs(aug[pivot][col]))
                pivot = row;
        }
        if (pivot != col) {
            for (int j = 0; j < 4; ++j) {
                double tmp    = aug[col][j];
                aug[col][j]   = aug[pivot][j];
                aug[pivot][j] = tmp;
            }
        }
        if (std::fabs(aug[col][col]) < 1e-10) return false;  // singular

        // Eliminate below.
        for (int row = col + 1; row < 3; ++row) {
            double factor = aug[row][col] / aug[col][col];
            for (int j = col; j < 4; ++j)
                aug[row][j] -= factor * aug[col][j];
        }
    }

    // Back-substitution.
    for (int row = 2; row >= 0; --row) {
        x[row] = aug[row][3];
        for (int j = row + 1; j < 3; ++j) x[row] -= aug[row][j] * x[j];
        x[row] /= aug[row][row];
    }
    return true;
}

// ---------------------------------------------------------------------------
// fit
// ---------------------------------------------------------------------------
bool fit(const std::vector<CalPoint>& points, TouchCalibration& out) {
    if (points.size() < 3) return false;

    // Accumulate the normal-equation sums for A^T A and A^T b.
    // Each row of A is [rx_i, ry_i, 1].
    double S_xx = 0, S_xy = 0, S_x = 0;
    double S_yy = 0, S_y  = 0;
    const double N = static_cast<double>(points.size());

    double ATbx[3] = {0, 0, 0};  // A^T * screen_x_vec
    double ATby[3] = {0, 0, 0};  // A^T * screen_y_vec

    for (const auto& p : points) {
        const double rx = p.raw_x;
        const double ry = p.raw_y;
        S_xx += rx * rx;
        S_xy += rx * ry;
        S_x  += rx;
        S_yy += ry * ry;
        S_y  += ry;
        ATbx[0] += rx * p.screen_x;
        ATbx[1] += ry * p.screen_x;
        ATbx[2] += p.screen_x;
        ATby[0] += rx * p.screen_y;
        ATby[1] += ry * p.screen_y;
        ATby[2] += p.screen_y;
    }

    // ATA is shared for both sub-problems; solve3 modifies it in place,
    // so keep two copies.
    double ATA[3][3] = {
        {S_xx, S_xy, S_x},
        {S_xy, S_yy, S_y},
        {S_x,  S_y,  N  }
    };
    double ATA2[3][3];
    std::memcpy(ATA2, ATA, sizeof(ATA));

    double cx[3], cy[3];
    if (!solve3(ATA,  ATbx, cx)) return false;
    if (!solve3(ATA2, ATby, cy)) return false;

    out.a = static_cast<float>(cx[0]);
    out.b = static_cast<float>(cx[1]);
    out.c = static_cast<float>(cx[2]);
    out.d = static_cast<float>(cy[0]);
    out.e = static_cast<float>(cy[1]);
    out.f = static_cast<float>(cy[2]);
    return true;
}

// ---------------------------------------------------------------------------
// apply
// ---------------------------------------------------------------------------
ScreenPoint apply(const TouchCalibration& cal,
                  int raw_x, int raw_y,
                  int screen_w, int screen_h) {
    float px = cal.a * static_cast<float>(raw_x)
             + cal.b * static_cast<float>(raw_y)
             + cal.c;
    float py = cal.d * static_cast<float>(raw_x)
             + cal.e * static_cast<float>(raw_y)
             + cal.f;

    int x = static_cast<int>(px);
    int y = static_cast<int>(py);

    if (x < 0)           x = 0;
    if (x >= screen_w)   x = screen_w - 1;
    if (y < 0)           y = 0;
    if (y >= screen_h)   y = screen_h - 1;

    return ScreenPoint{x, y};
}

// ---------------------------------------------------------------------------
// Serialization helpers: IEEE-754 single stored as 4 bytes, little-endian.
// ---------------------------------------------------------------------------
static void store_float_le(uint8_t* dst, float v) {
    uint32_t bits;
    std::memcpy(&bits, &v, sizeof(bits));
    dst[0] = static_cast<uint8_t>( bits        & 0xFF);
    dst[1] = static_cast<uint8_t>((bits >>  8) & 0xFF);
    dst[2] = static_cast<uint8_t>((bits >> 16) & 0xFF);
    dst[3] = static_cast<uint8_t>((bits >> 24) & 0xFF);
}

static float load_float_le(const uint8_t* src) {
    uint32_t bits = (static_cast<uint32_t>(src[0])      )
                  | (static_cast<uint32_t>(src[1]) <<  8)
                  | (static_cast<uint32_t>(src[2]) << 16)
                  | (static_cast<uint32_t>(src[3]) << 24);
    float v;
    std::memcpy(&v, &bits, sizeof(v));
    return v;
}

// ---------------------------------------------------------------------------
// to_bytes / from_bytes
// ---------------------------------------------------------------------------
std::vector<uint8_t> to_bytes(const TouchCalibration& cal) {
    std::vector<uint8_t> blob(kTouchCalBlobSize, 0);
    blob[0] = kTouchCalVersion;
    store_float_le(&blob[ 1], cal.a);
    store_float_le(&blob[ 5], cal.b);
    store_float_le(&blob[ 9], cal.c);
    store_float_le(&blob[13], cal.d);
    store_float_le(&blob[17], cal.e);
    store_float_le(&blob[21], cal.f);
    return blob;
}

bool from_bytes(const std::vector<uint8_t>& blob, TouchCalibration& out) {
    if (blob.size() < kTouchCalBlobSize)    return false;
    if (blob[0] != kTouchCalVersion)        return false;
    out.a = load_float_le(&blob[ 1]);
    out.b = load_float_le(&blob[ 5]);
    out.c = load_float_le(&blob[ 9]);
    out.d = load_float_le(&blob[13]);
    out.e = load_float_le(&blob[17]);
    out.f = load_float_le(&blob[21]);
    return true;
}

}  // namespace osp
