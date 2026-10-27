#ifndef UC2HIK_TEST_IMAGE_HELPERS_H
#define UC2HIK_TEST_IMAGE_HELPERS_H

// Image analysis shared by the tests of the simulated spot (test_camera_helpers,
// test_simcamera, test_worker).

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace testimg {

/// Centre of the spot in a single-channel @p w x @p h image.
///
/// The simulated image has an offset and noise, so a plain intensity-weighted
/// centroid would be pulled towards the image centre. Here the background is
/// the median, and a sample weighs max(v - bg - 0.2 (max - bg), 0): only the
/// spot counts, and its weight falls to zero at the cut. For a symmetric spot
/// this is unbiased.
template <typename T>
void spotCentroid(const T* img, int64_t w, int64_t h, double& cx, double& cy)
{
    const size_t   n = static_cast<size_t>(w) * static_cast<size_t>(h);
    std::vector<T> sorted(img, img + n);
    std::nth_element(sorted.begin(), sorted.begin() + static_cast<std::ptrdiff_t>(n / 2), sorted.end());
    const double bg  = sorted[n / 2];
    const double top = *std::max_element(img, img + n);
    const double cut = bg + 0.2 * (top - bg);

    double sum = 0, sx = 0, sy = 0;
    for (int64_t y = 0; y < h; ++y)
    {
        for (int64_t x = 0; x < w; ++x)
        {
            const double v = img[y * w + x] - cut;
            if (v > 0)
            {
                sum += v; sx += v * x; sy += v * y;
            }
        }
    }
    cx = sx / sum;
    cy = sy / sum;
}

/// Mean and variance of all samples farther than @p radius from (@p cx, @p cy).
template <typename T>
void backgroundStats(const T* img, int64_t w, int64_t h, double cx, double cy, double radius,
                     double& mean, double& var)
{
    double n = 0, s = 0, s2 = 0;
    for (int64_t y = 0; y < h; ++y)
    {
        for (int64_t x = 0; x < w; ++x)
        {
            if (std::hypot(x - cx, y - cy) > radius)
            {
                const double v = img[y * w + x];
                n += 1; s += v; s2 += v * v;
            }
        }
    }
    mean = s / n;
    var  = s2 / n - mean * mean;
}

} // namespace testimg

#endif // UC2HIK_TEST_IMAGE_HELPERS_H
