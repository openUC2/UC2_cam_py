#include "SimPattern.h"

#include <cmath>
#include <vector>

namespace hik {
namespace sim {

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
    const uint32_t b = (binning < 1) ? 1 : binning;
    const int64_t  w = roi.width / b;
    const int64_t  h = roi.height / b;
    if (dst == nullptr || w <= 0 || h <= 0)
    {
        return;
    }

    // The Gaussian is separable: two 1-D profiles, one multiplication per pixel.
    const double sigma = fwhmPx / (2.0 * std::sqrt(2.0 * std::log(2.0)));
    const double k     = -1.0 / (2.0 * sigma * sigma);
    // Centre of the sensor pixels covered by binned pixel i: roi + i*b + (b-1)/2.
    const double half  = (static_cast<double>(b) - 1.0) / 2.0;

    std::vector<double> gx(static_cast<size_t>(w));
    std::vector<double> gy(static_cast<size_t>(h));
    for (int64_t i = 0; i < w; ++i)
    {
        const double xs = static_cast<double>(roi.x) + static_cast<double>(i) * b + half - c.x;
        gx[static_cast<size_t>(i)] = std::exp(k * xs * xs);
    }
    for (int64_t j = 0; j < h; ++j)
    {
        const double ys = static_cast<double>(roi.y) + static_cast<double>(j) * b + half - c.y;
        gy[static_cast<size_t>(j)] = peak * std::exp(k * ys * ys);
    }

    if (depth == PixelDepth::UInt16)
    {
        uint16_t* out = static_cast<uint16_t*>(dst);
        for (int64_t j = 0; j < h; ++j)
        {
            uint16_t* row = out + static_cast<size_t>(j) * static_cast<size_t>(w);
            for (int64_t i = 0; i < w; ++i)
            {
                row[i] = static_cast<uint16_t>(gy[static_cast<size_t>(j)] * gx[static_cast<size_t>(i)] + 0.5);
            }
        }
    }
    else
    {
        uint8_t* out = static_cast<uint8_t*>(dst);
        for (int64_t j = 0; j < h; ++j)
        {
            uint8_t* row = out + static_cast<size_t>(j) * static_cast<size_t>(w);
            for (int64_t i = 0; i < w; ++i)
            {
                row[i] = static_cast<uint8_t>(gy[static_cast<size_t>(j)] * gx[static_cast<size_t>(i)] + 0.5);
            }
        }
    }
}

} // namespace sim
} // namespace hik
