#include "SimPattern.h"

#include <algorithm>
#include <cmath>
#include <random>
#include <vector>

namespace hik {
namespace sim {

namespace {

/// The Gaussian is separable: the spot value of sample (i, j) is gy[j] * gx[i],
/// one multiplication per pixel. gy carries the peak.
struct SpotProfiles
{
    std::vector<double> gx;
    std::vector<double> gy;
};

/// @return false if the image is empty.
bool spotProfiles(const SensorRoi& roi, uint32_t binning, const SpotPosition& c,
                  double fwhmPx, double peak, SpotProfiles& p)
{
    const uint32_t b = (binning < 1) ? 1 : binning;
    const int64_t  w = roi.width / b;
    const int64_t  h = roi.height / b;
    if (w <= 0 || h <= 0)
    {
        return false;
    }

    const double sigma = fwhmPx / (2.0 * std::sqrt(2.0 * std::log(2.0)));
    const double k     = -1.0 / (2.0 * sigma * sigma);
    // Centre of the sensor pixels covered by binned pixel i: roi + i*b + (b-1)/2.
    const double half  = (static_cast<double>(b) - 1.0) / 2.0;

    p.gx.resize(static_cast<size_t>(w));
    p.gy.resize(static_cast<size_t>(h));
    for (int64_t i = 0; i < w; ++i)
    {
        const double xs = static_cast<double>(roi.x) + static_cast<double>(i) * b + half - c.x;
        p.gx[static_cast<size_t>(i)] = std::exp(k * xs * xs);
    }
    for (int64_t j = 0; j < h; ++j)
    {
        const double ys = static_cast<double>(roi.y) + static_cast<double>(j) * b + half - c.y;
        p.gy[static_cast<size_t>(j)] = peak * std::exp(k * ys * ys);
    }
    return true;
}

template <typename T>
void drawExact(T* out, const SpotProfiles& p)
{
    const size_t w = p.gx.size();
    for (size_t j = 0; j < p.gy.size(); ++j)
    {
        T* row = out + j * w;
        for (size_t i = 0; i < w; ++i)
        {
            row[i] = static_cast<T>(p.gy[j] * p.gx[i] + 0.5);
        }
    }
}

/// Normal approximation in float with a 32-bit engine: about 26 ms for a
/// 1920 x 1200 frame on x86, exact std::poisson_distribution takes 330 ms.
template <typename T>
void drawNoisy(T* out, const SpotProfiles& p, double offset, float maxValue, std::mt19937& rng)
{
    std::normal_distribution<float> z(0.0f, 1.0f);
    const size_t w = p.gx.size();
    for (size_t j = 0; j < p.gy.size(); ++j)
    {
        T* row = out + j * w;
        for (size_t i = 0; i < w; ++i)
        {
            const float lambda = std::max(0.0f, static_cast<float>(offset + p.gy[j] * p.gx[i]));
            const float v      = lambda + std::sqrt(lambda) * z(rng);
            row[i] = static_cast<T>(v <= 0.0f ? 0.0f : (v >= maxValue ? maxValue : v + 0.5f));
        }
    }
}

} // namespace

SpotPosition spotPosition(uint64_t index, uint32_t sensorWidth, uint32_t sensorHeight,
                          uint32_t cols, uint32_t rows)
{
    if (cols == 0) { cols = 1; }
    if (rows == 0) { rows = 1; }
    const uint64_t col = index % cols;
    const uint64_t row = (index / cols) % rows;
    return SpotPosition{ sensorWidth * (col + 0.5) / cols, sensorHeight * (row + 0.5) / rows };
}

void renderSpot(void* dst, PixelDepth depth, const SensorRoi& roi, uint32_t binning,
                const SpotPosition& c, double fwhmPx, double peak)
{
    SpotProfiles p;
    if (dst == nullptr || !spotProfiles(roi, binning, c, fwhmPx, peak, p))
    {
        return;
    }
    if (depth == PixelDepth::UInt16)
    {
        drawExact(static_cast<uint16_t*>(dst), p);
    }
    else
    {
        drawExact(static_cast<uint8_t*>(dst), p);
    }
}

void renderNoisySpot(void* dst, PixelDepth depth, const SensorRoi& roi, uint32_t binning,
                     const SpotPosition& c, double fwhmPx, double peak, double offset,
                     uint64_t seed)
{
    SpotProfiles p;
    if (dst == nullptr || !spotProfiles(roi, binning, c, fwhmPx, peak, p))
    {
        return;
    }
    std::seed_seq seq{ static_cast<uint32_t>(seed), static_cast<uint32_t>(seed >> 32) };
    std::mt19937  rng(seq);
    if (depth == PixelDepth::UInt16)
    {
        drawNoisy(static_cast<uint16_t*>(dst), p, offset, 65535.0f, rng);
    }
    else
    {
        drawNoisy(static_cast<uint8_t*>(dst), p, offset, 255.0f, rng);
    }
}

} // namespace sim
} // namespace hik
