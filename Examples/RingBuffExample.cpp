// RingBuffExample - manual end-to-end test of the UC2 ring buffer without a camera.
//
// A producer thread renders 20 synthetic Mono16 frames (1920 x 1200) showing a
// Gaussian spot (FWHM 100 px) that walks across the image in 4 rows of 5
// positions. Two consumer threads read the ring concurrently:
//
//   - the analysis consumer is registered and reads every frame by sequence
//     number, computes the spot's centroid and checks it against the position
//     the producer used;
//   - the display consumer is anonymous, polls the latest frame and shows it,
//     scaled down to 960 x 600 and reduced to 8 bit, in an ImageWindow.
//
// In parallel the ring buffer's built-in FileStreamSink writes every frame to a
// .uc2rb container (plus .json sidecar) on its own writer thread.
//
// Usage: RingBuffExample [storageDir]      (default: /data/recordings)
//
// The program ends once the preview window is closed (immediately when no
// display is available). Exit code 0 means every frame arrived intact, was at
// the expected position and was saved to disk.

#include "ImageWindow.h"
#include "UC2_RingBuff.h"
#include "hikLog.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {

// --- Scenario --------------------------------------------------------------

const uint32_t kWidth     = 1920;
const uint32_t kHeight    = 1200;
const uint32_t kCols      = 5;        ///< spot positions per row
const uint32_t kRows      = 4;        ///< rows the spot walks through
const uint32_t kFrames    = kCols * kRows;
const double   kFwhmPx    = 100.0;    ///< full width at half maximum of the spot
const double   kPeak      = 60000.0;  ///< spot amplitude in counts (background is 0)
const double   kTolPx     = 0.5;      ///< allowed centroid error

const std::chrono::milliseconds kFrameInterval(200);   ///< producer rate: 5 fps
const std::chrono::milliseconds kDisplayPoll(30);

const uint32_t kViewScale  = 2;       ///< display shows the frame at 1/kViewScale
const uint32_t kViewWidth  = kWidth / kViewScale;
const uint32_t kViewHeight = kHeight / kViewScale;

struct Point
{
    double x;
    double y;
};

/// Spot centre for frame @p index: row-major over a kCols x kRows grid whose
/// cells evenly divide the image, the spot sitting in the middle of each cell.
Point expectedPosition(uint64_t index)
{
    const uint64_t col = index % kCols;
    const uint64_t row = (index / kCols) % kRows;
    return Point{ kWidth * (col + 0.5) / kCols, kHeight * (row + 0.5) / kRows };
}

/// Render the Gaussian spot centred at @p c into @p dst (kWidth x kHeight).
/// The 2-D Gaussian is separable, so two 1-D profiles are computed once and
/// every pixel is a single multiplication.
void renderGaussian(uint16_t* dst, const Point& c)
{
    const double sigma = kFwhmPx / (2.0 * std::sqrt(2.0 * std::log(2.0)));
    const double k     = -1.0 / (2.0 * sigma * sigma);

    std::vector<double> gx(kWidth);
    std::vector<double> gy(kHeight);
    for (uint32_t x = 0; x < kWidth; ++x)
    {
        gx[x] = std::exp(k * (x - c.x) * (x - c.x));
    }
    for (uint32_t y = 0; y < kHeight; ++y)
    {
        gy[y] = kPeak * std::exp(k * (y - c.y) * (y - c.y));
    }
    for (uint32_t y = 0; y < kHeight; ++y)
    {
        uint16_t* row = dst + static_cast<size_t>(y) * kWidth;
        for (uint32_t x = 0; x < kWidth; ++x)
        {
            row[x] = static_cast<uint16_t>(gy[y] * gx[x] + 0.5);
        }
    }
}

/// Intensity-weighted centroid of a kWidth x kHeight Mono16 image.
Point centroid(const uint16_t* src)
{
    double sum = 0.0, sx = 0.0, sy = 0.0;
    for (uint32_t y = 0; y < kHeight; ++y)
    {
        const uint16_t* row = src + static_cast<size_t>(y) * kWidth;
        double rowSum = 0.0, rowSx = 0.0;
        for (uint32_t x = 0; x < kWidth; ++x)
        {
            rowSum += row[x];
            rowSx  += static_cast<double>(row[x]) * x;
        }
        sum += rowSum;
        sx  += rowSx;
        sy  += rowSum * y;
    }
    if (sum <= 0.0)
    {
        return Point{ -1.0, -1.0 };
    }
    return Point{ sx / sum, sy / sum };
}

/// Shrink a Mono16 frame by kViewScale (box average) and map it to 8 bit.
void toView(const uint16_t* src, std::vector<unsigned char>& view)
{
    const uint32_t area = kViewScale * kViewScale;
    view.resize(static_cast<size_t>(kViewWidth) * kViewHeight);
    for (uint32_t vy = 0; vy < kViewHeight; ++vy)
    {
        for (uint32_t vx = 0; vx < kViewWidth; ++vx)
        {
            uint32_t acc = 0;
            for (uint32_t dy = 0; dy < kViewScale; ++dy)
            {
                const uint16_t* row = src + static_cast<size_t>(vy * kViewScale + dy) * kWidth
                                          + vx * kViewScale;
                for (uint32_t dx = 0; dx < kViewScale; ++dx)
                {
                    acc += row[dx];
                }
            }
            view[static_cast<size_t>(vy) * kViewWidth + vx] =
                static_cast<unsigned char>((acc / area) >> 8);
        }
    }
}

// --- Threads ---------------------------------------------------------------

/// Shared state between the threads. The ring buffer carries the frames; this
/// only carries "who is done".
struct Shared
{
    std::atomic<bool>     producerDone{ false };
    std::atomic<uint32_t> framesOk{ 0 };
    std::atomic<uint32_t> framesBad{ 0 };    ///< wrong position or torn while reading
    std::atomic<uint32_t> framesLost{ 0 };   ///< recycled before the analysis got to it
};

void producerThread(hik::RingBuffer& rb, Shared& shared)
{
    auto next = std::chrono::steady_clock::now();
    for (uint32_t i = 0; i < kFrames; ++i)
    {
        std::this_thread::sleep_until(next);
        next += kFrameInterval;

        hik::RingBuffer::WriteHandle w;
        if (!rb.beginWrite(w))
        {
            hik::ERR("producer: beginWrite failed for frame " + std::to_string(i));
            continue;
        }
        // Render straight into the slot -- this stands in for the camera's copy.
        renderGaussian(reinterpret_cast<uint16_t*>(w.data()), expectedPosition(w.sequence()));
        if (!rb.commitWrite(w, w.capacity()))
        {
            hik::ERR("producer: commitWrite failed for frame " + std::to_string(i));
        }
    }
    shared.producerDone.store(true);
}

void analysisThread(hik::RingBuffer& rb, uint32_t consumer, Shared& shared)
{
    std::cout << "\n  seq  slot   expected (x, y)      centroid (x, y)       error   write [ms]  status\n";

    for (uint64_t seq = 0; seq < kFrames; ++seq)
    {
        // Wait until the producer has published this frame.
        while (rb.framesWritten() <= seq)
        {
            if (shared.producerDone.load() && rb.framesWritten() <= seq)
            {
                hik::ERR("analysis: producer finished before frame " + std::to_string(seq));
                shared.framesLost.fetch_add(1);
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        hik::RingBuffer::ReadHandle h;
        if (!rb.readSequence(seq, h, consumer))
        {
            // Overwrite policy: the slot already holds a newer frame. Acknowledge
            // it anyway so the consumer cursor keeps moving.
            rb.acknowledge(consumer, seq);
            shared.framesLost.fetch_add(1);
            std::cout << std::setw(5) << seq << "  frame lost (recycled before it was read)\n";
            continue;
        }

        const Point  want  = expectedPosition(seq);
        const Point  got   = centroid(reinterpret_cast<const uint16_t*>(h.data()));
        const double err   = std::hypot(got.x - want.x, got.y - want.y);
        const bool   valid = h.stillValid();   // after consuming the bytes, not before
        const double ms    = (h.tsAfterNs() - h.tsBeforeNs()) / 1e6;
        const bool   ok    = valid && err <= kTolPx;

        std::cout << std::fixed << std::setprecision(2)
                  << std::setw(5) << seq << std::setw(6) << h.slot()
                  << "   (" << std::setw(7) << want.x << ", " << std::setw(7) << want.y << ")"
                  << "   (" << std::setw(7) << got.x << ", " << std::setw(7) << got.y << ")"
                  << std::setw(8) << err << std::setw(12) << ms << "  "
                  << (ok ? "OK" : (valid ? "POSITION" : "TORN")) << "\n";

        (ok ? shared.framesOk : shared.framesBad).fetch_add(1);
        // ~ReadHandle marks the frame read and advances this consumer's cursor.
    }
}

void displayThread(hik::RingBuffer& rb, Shared& shared)
{
    // Created here, not in main: Xlib runs without XInitThreads(), so the window
    // must live on the one thread that uses it.
    hik::ImageWindow window(kViewWidth, kViewHeight, "RingBuff Example - TEST");
    if (!window.isValid())
    {
        hik::WARN("display: no window available, running without preview");
        return;
    }

    std::vector<unsigned char> view;
    uint64_t shown       = UINT64_MAX;
    bool     hintPrinted = false;

    while (window.pumpEvents())
    {
        {
            // Scoped so the slot is never left pinned across the poll sleep.
            hik::RingBuffer::ReadHandle h;
            if (rb.readLatest(h) && h.sequence() != shown)
            {
                toView(reinterpret_cast<const uint16_t*>(h.data()), view);
                const uint64_t seq   = h.sequence();
                const bool     valid = h.stillValid();
                h.release();   // unpin before the (slow) X11 upload
                if (valid)
                {
                    window.showGray8(view.data(), kViewWidth, kViewHeight);
                    shown = seq;
                }
            }
        }

        if (!hintPrinted && shared.producerDone.load() && shown == kFrames - 1)
        {
            hik::INFO("display: all frames shown - close the window to exit");
            hintPrinted = true;
        }
        std::this_thread::sleep_for(kDisplayPoll);
    }
}

} // namespace

int main(int argc, char** argv)
{
    hik::RingBuffer::Config cfg;
    cfg.storagePath = (argc > 1) ? argv[1] : "/data/recordings";
    cfg.slotCount   = 8;
    cfg.comment     = "bead tracking run 7";
    cfg.policy      = hik::FullPolicy::Overwrite;

    hik::RingBuffer rb(cfg);
    rb.setImageSize(kWidth, kHeight);
    rb.setChannels(1);
    rb.setDepth(hik::PixelDepth::UInt16);
    rb.setCameraId("TEST");

    if (!rb.configure())
    {
        hik::ERR("RingBuffExample: configure() failed");
        return 1;
    }
    if (!rb.startRecording())
    {
        hik::ERR("RingBuffExample: startRecording() failed - is '" + cfg.storagePath
                 + "' writable? Create it with\n"
                   "    sudo mkdir -p " + cfg.storagePath + " && sudo chown $USER " + cfg.storagePath
                 + "\nor pass another directory as the first argument.");
        return 1;
    }

    // Register before the producer starts so the analysis cursor begins at 0.
    const uint32_t analysisId = rb.registerConsumer();

    Shared shared;
    std::thread display(displayThread, std::ref(rb), std::ref(shared));
    std::thread analysis(analysisThread, std::ref(rb), analysisId, std::ref(shared));
    std::thread producer(producerThread, std::ref(rb), std::ref(shared));

    producer.join();
    analysis.join();
    rb.unregisterConsumer(analysisId);
    rb.stopRecording();   // lets the disk writer drain, then fsyncs and closes the file

    const bool diskOk = !rb.streamHasError() && rb.streamFramesSaved() == kFrames;
    std::cout << "\nRing buffer\n"
              << "  frames written     " << rb.framesWritten() << "\n"
              << "  frames overwritten " << rb.framesOverwritten() << "\n"
              << "  rejected reads     " << rb.rejectedReads() << "\n"
              << "Analysis consumer\n"
              << "  ok / bad / lost    " << shared.framesOk.load() << " / "
              << shared.framesBad.load() << " / " << shared.framesLost.load() << "\n"
              << "Disk streamer\n"
              << "  file               " << rb.streamFilePath() << "\n"
              << "  saved / dropped    " << rb.streamFramesSaved() << " / "
              << rb.streamFramesDropped() << (rb.streamHasError() ? "  (write error)" : "") << "\n"
              << std::endl;

    display.join();   // returns once the preview window is closed

    const bool ok = shared.framesOk.load() == kFrames && diskOk;
    std::cout << (ok ? "RESULT: PASS" : "RESULT: FAIL") << std::endl;
    return ok ? 0 : 1;
}
