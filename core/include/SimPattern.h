#ifndef UC2HIK_SIMPATTERN_H
#define UC2HIK_SIMPATTERN_H

#include <cstdint>

#include "CameraBase.h"

/// @file SimPattern.h
/// @brief Synthetic test image: a Gaussian spot that walks across the sensor.
///
/// Shared by SimCamera and Examples/RingBuffExample.cpp. The spot visits a
/// cols x rows grid row by row (5 x 4 by default); every grid cell divides the
/// sensor evenly and the spot sits in its centre, so the expected centroid of
/// frame k is known exactly and tests can check the image content.
///
/// renderSpot() draws the exact spot on a background of 0; renderNoisySpot()
/// adds a constant offset and shot noise, as SimCamera uses it.

namespace hik {
namespace sim {

/// Centre of the spot in unbinned sensor pixel coordinates.
struct SpotPosition
{
    double x;
    double y;
};

/// @return the spot centre for frame @p index on a @p sensorWidth x
///         @p sensorHeight sensor (pixel centres at integer coordinates).
SpotPosition spotPosition(uint64_t index, uint32_t sensorWidth, uint32_t sensorHeight,
                          uint32_t cols = 5, uint32_t rows = 4);

/// Render the spot centred at @p c into a single-channel image that covers the
/// sensor region @p roi (unbinned sensor pixels) at @p binning: the image is
/// (roi.width / binning) x (roi.height / binning) samples, row-major. Each
/// binned pixel takes the spot value at the centre of the sensor pixels it
/// covers. Background is 0, the peak is @p peak.
///
/// @param dst     uint8_t* (depth UInt8) or uint16_t* (depth UInt16) buffer
/// @param depth   sample type of @p dst
void renderSpot(void* dst, PixelDepth depth, const SensorRoi& roi, uint32_t binning,
                const SpotPosition& c, double fwhmPx, double peak);

/// Like renderSpot(), on a constant @p offset and with shot noise: each sample
/// is drawn from N(lambda, lambda) with lambda = offset + spot value, i.e. the
/// Poisson statistics (variance = mean) in their normal approximation, rounded
/// and limited to the range of @p depth. The same @p seed gives the same image
/// (with the same standard library).
void renderNoisySpot(void* dst, PixelDepth depth, const SensorRoi& roi, uint32_t binning,
                     const SpotPosition& c, double fwhmPx, double peak, double offset,
                     uint64_t seed);

} // namespace sim
} // namespace hik

#endif // UC2HIK_SIMPATTERN_H
