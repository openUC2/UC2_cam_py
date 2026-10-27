// Shared-memory ring buffer: a producer allocates its region in a sealed memfd,
// a reader maps it with RingBuffer::attach(). The properties under test are the
// ones the camera process split depends on: the reader sees the producer's
// frames, it can never be made to read outside its mapping (sealing, header
// validation, its own copy of the geometry), and its mapping survives the
// producer -- also a producer process that is killed mid-frame.
#include <catch2/catch_test_macros.hpp>

#include <fcntl.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <functional>
#include <memory>
#include <thread>
#include <vector>

#include "UC2_RingBuff.h"

using hik::PixelDepth;
using hik::RingBuffer;

namespace {

const uint32_t kW = 64;
const uint32_t kH = 32;

RingBuffer::Config shmConfig(uint32_t slots = 4)
{
    RingBuffer::Config c;
    c.slotCount    = slots;
    c.comment      = "shm test";
    c.sharedMemory = true;
    return c;
}

bool configureSmall(RingBuffer& rb, uint32_t w = kW, uint32_t h = kH)
{
    return rb.setImageSize(w, h) && rb.setChannels(1) && rb.setDepth(PixelDepth::UInt16)
        && rb.setCameraId("SHM-TEST") && rb.configure();
}

/// Stamp @p seq into the first and last eight bytes and paint the rest with a
/// byte derived from it, so that any mixture of two frames is detectable.
void paint(uint8_t* dst, uint64_t nbytes, uint64_t seq)
{
    std::memset(dst, static_cast<int>(seq & 0xFF), static_cast<size_t>(nbytes));
    std::memcpy(dst, &seq, sizeof(seq));
    std::memcpy(dst + nbytes - sizeof(seq), &seq, sizeof(seq));
}

/// @return true if @p data is a complete, untorn frame carrying @p seq.
bool intact(const uint8_t* data, uint64_t nbytes, uint64_t seq)
{
    uint64_t head = 0;
    uint64_t tail = 0;
    std::memcpy(&head, data, sizeof(head));
    std::memcpy(&tail, data + nbytes - sizeof(tail), sizeof(tail));
    return head == seq && tail == seq && data[nbytes / 2] == static_cast<uint8_t>(seq & 0xFF);
}

bool writePainted(RingBuffer& rb)
{
    RingBuffer::WriteHandle w;
    if (!rb.beginWrite(w))
    {
        return false;
    }
    paint(w.data(), w.capacity(), w.sequence());
    return rb.commitWrite(w, w.capacity());
}

/// Copy of the whole region behind @p fd.
std::vector<uint8_t> regionBytes(int fd, size_t size)
{
    void* p = ::mmap(nullptr, size, PROT_READ, MAP_SHARED, fd, 0);
    REQUIRE(p != MAP_FAILED);
    std::vector<uint8_t> out(static_cast<const uint8_t*>(p), static_cast<const uint8_t*>(p) + size);
    ::munmap(p, size);
    return out;
}

/// A new memfd holding @p bytes, sealed against resizing if @p seal.
int memfdWith(const std::vector<uint8_t>& bytes, bool seal = true)
{
    const int fd = ::memfd_create("forged", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    REQUIRE(fd >= 0);
    REQUIRE(::write(fd, bytes.data(), bytes.size()) == static_cast<ssize_t>(bytes.size()));
    if (seal)
    {
        REQUIRE(::fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_SEAL) == 0);
    }
    return fd;
}

/// attach() a copy of @p src's region after @p mutate changed it.
std::shared_ptr<RingBuffer> attachForged(const RingBuffer& src,
                                         const std::function<void(std::vector<uint8_t>&)>& mutate,
                                         bool seal = true)
{
    std::vector<uint8_t> bytes = regionBytes(src.sharedFd(), static_cast<size_t>(src.regionBytes()));
    mutate(bytes);
    const int fd = memfdWith(bytes, seal);
    std::shared_ptr<RingBuffer> rb = RingBuffer::attach(fd);
    ::close(fd);
    return rb;
}

hik::UC2_SessionHeader& headerIn(std::vector<uint8_t>& bytes)
{
    return *reinterpret_cast<hik::UC2_SessionHeader*>(bytes.data());
}

// --- passing the memfd to another process, as the camera process does ------

bool sendFd(int sock, int fd)
{
    char byte = 'F';
    struct iovec iov;
    iov.iov_base = &byte;
    iov.iov_len  = 1;
    alignas(struct cmsghdr) char ctrl[CMSG_SPACE(sizeof(int))];
    std::memset(ctrl, 0, sizeof(ctrl));
    struct msghdr msg;
    std::memset(&msg, 0, sizeof(msg));
    msg.msg_iov        = &iov;
    msg.msg_iovlen     = 1;
    msg.msg_control    = ctrl;
    msg.msg_controllen = sizeof(ctrl);
    struct cmsghdr* c = CMSG_FIRSTHDR(&msg);
    c->cmsg_level = SOL_SOCKET;
    c->cmsg_type  = SCM_RIGHTS;
    c->cmsg_len   = CMSG_LEN(sizeof(int));
    std::memcpy(CMSG_DATA(c), &fd, sizeof(int));
    return ::sendmsg(sock, &msg, MSG_NOSIGNAL) == 1;
}

int recvFd(int sock)
{
    char byte = 0;
    struct iovec iov;
    iov.iov_base = &byte;
    iov.iov_len  = 1;
    alignas(struct cmsghdr) char ctrl[CMSG_SPACE(sizeof(int))];
    struct msghdr msg;
    std::memset(&msg, 0, sizeof(msg));
    msg.msg_iov        = &iov;
    msg.msg_iovlen     = 1;
    msg.msg_control    = ctrl;
    msg.msg_controllen = sizeof(ctrl);
    if (::recvmsg(sock, &msg, MSG_CMSG_CLOEXEC) != 1)
    {
        return -1;
    }
    struct cmsghdr* c = CMSG_FIRSTHDR(&msg);
    if (c == nullptr || c->cmsg_type != SCM_RIGHTS)
    {
        return -1;
    }
    int fd = -1;
    std::memcpy(&fd, CMSG_DATA(c), sizeof(int));
    return fd;
}

/// Child process body: create a shared ring, send its memfd over @p sock, wait
/// @p delayMs, then write @p frames painted frames (0 = until killed), @p paceUs
/// apart. Never returns.
[[noreturn]] void runProducerChild(int sock, uint64_t frames, int delayMs, int paceUs)
{
    {
        RingBuffer rb(shmConfig(8));
        if (!configureSmall(rb, 128, 128) || !sendFd(sock, rb.sharedFd()))
        {
            ::_exit(2);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
        for (uint64_t i = 0; frames == 0 || i < frames; ++i)
        {
            writePainted(rb);
            if (paceUs > 0)
            {
                std::this_thread::sleep_for(std::chrono::microseconds(paceUs));
            }
        }
    }
    ::_exit(0);   // no Catch2 teardown in the child
}

struct Child
{
    pid_t pid  = -1;
    int   sock = -1;
};

/// Fork a producer child (see runProducerChild) and return its pid plus our
/// end of the socket. The test process has no other threads at this point.
Child forkProducer(uint64_t frames, int delayMs, int paceUs)
{
    int sv[2];
    REQUIRE(::socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv) == 0);
    const pid_t pid = ::fork();
    REQUIRE(pid >= 0);
    if (pid == 0)
    {
        ::close(sv[0]);
        runProducerChild(sv[1], frames, delayMs, paceUs);
    }
    ::close(sv[1]);
    Child c;
    c.pid  = pid;
    c.sock = sv[0];
    return c;
}

int reap(pid_t pid)
{
    int status = 0;
    ::waitpid(pid, &status, 0);
    return status;
}

} // namespace

// ---------------------------------------------------------------------------
// The producer side
// ---------------------------------------------------------------------------

TEST_CASE("a shared-memory ring lives in a memfd sealed against resizing", "[shm]")
{
    RingBuffer rb(shmConfig());
    CHECK(rb.sharedFd() == -1);   // not configured yet
    REQUIRE(configureSmall(rb));

    const int fd = rb.sharedFd();
    REQUIRE(fd >= 0);
    const int seals = ::fcntl(fd, F_GET_SEALS);
    CHECK((seals & F_SEAL_SHRINK) != 0);
    CHECK((seals & F_SEAL_GROW) != 0);
    CHECK((seals & F_SEAL_SEAL) != 0);   // nobody can lift the seals again

    // Exactly what a reader relies on: the size can never change.
    errno = 0;
    CHECK(::ftruncate(fd, 4096) != 0);
    CHECK(errno == EPERM);
    CHECK(::ftruncate(fd, static_cast<off_t>(rb.regionBytes()) + 4096) != 0);
    CHECK((::fcntl(fd, F_GETFD) & FD_CLOEXEC) != 0);   // not inherited by spawned programs
}

TEST_CASE("a heap ring has no shared fd and refuses to mix with the memfd provider", "[shm]")
{
    RingBuffer heap(RingBuffer::Config{});
    REQUIRE(configureSmall(heap));
    CHECK(heap.sharedFd() == -1);
    CHECK(RingBuffer::attach(heap.sharedFd()) == nullptr);

    RingBuffer shm(shmConfig());
    CHECK_FALSE(shm.setMemoryOps(hik::heapMemoryOps()));
}

// ---------------------------------------------------------------------------
// Attaching
// ---------------------------------------------------------------------------

TEST_CASE("an attached ring reads the producer's frames without a copy", "[shm]")
{
    RingBuffer rb(shmConfig(4));
    REQUIRE(configureSmall(rb));
    for (int i = 0; i < 3; ++i)
    {
        REQUIRE(writePainted(rb));
    }

    std::shared_ptr<RingBuffer> view = RingBuffer::attach(rb.sharedFd());
    REQUIRE(view);
    CHECK(view->isAttached());
    CHECK(view->sharedFd() == -1);
    CHECK(view->width() == kW);
    CHECK(view->height() == kH);
    CHECK(view->channels() == 1);
    CHECK(view->depth() == PixelDepth::UInt16);
    CHECK(view->frameBytes() == rb.frameBytes());
    CHECK(view->slotCount() == 4);
    CHECK(view->regionBytes() == rb.regionBytes());
    CHECK(view->cameraId() == "SHM-TEST");
    CHECK(view->comment() == "shm test");
    CHECK(view->framesWritten() == 3);

    RingBuffer::ReadHandle r;
    REQUIRE(view->readLatest(r));
    CHECK(r.sequence() == 2);
    CHECK(r.nbytes() == rb.frameBytes());
    CHECK(intact(r.data(), r.nbytes(), 2));
    r.release();

    REQUIRE(view->readSlot(0, r));
    CHECK(r.sequence() == 0);
    CHECK(intact(r.data(), r.nbytes(), 0));
    r.release();

    // The views share the pages: a new frame is visible at once, and recycling
    // a slot invalidates a handle held on the other side.
    RingBuffer::ReadHandle held;
    REQUIRE(view->readSequence(1, held));
    for (int i = 0; i < 4; ++i)
    {
        REQUIRE(writePainted(rb));
    }
    CHECK(view->framesWritten() == 7);
    REQUIRE(view->readLatest(r));
    CHECK(r.sequence() == 6);
    CHECK(intact(r.data(), r.nbytes(), 6));
    CHECK_FALSE(held.stillValid());
}

TEST_CASE("an attached ring can only be read", "[shm]")
{
    RingBuffer rb(shmConfig());
    REQUIRE(configureSmall(rb));
    REQUIRE(rb.startRecording());
    REQUIRE(writePainted(rb));

    {
        std::shared_ptr<RingBuffer> view = RingBuffer::attach(rb.sharedFd());
        REQUIRE(view);
        RingBuffer::WriteHandle w;
        CHECK_FALSE(view->beginWrite(w));
        CHECK_FALSE(view->writeFrame("x", 1));
        CHECK_FALSE(view->startRecording());
        CHECK_FALSE(view->startStreaming("/tmp"));
        CHECK(view->registerConsumer() == hik::kAnonymousConsumer);
        CHECK_FALSE(view->addSink(hik::UC2_SinkOps{ hik::kAbiVersion, nullptr, nullptr, nullptr,
                                                    nullptr, nullptr }));
        CHECK(view->isRecording());   // reports the producer's session ...
        view->stopRecording();        // ... but cannot end it
    }   // destroying the view must not end it either

    CHECK(rb.isRecording());
    REQUIRE(writePainted(rb));
    rb.stopRecording();
}

TEST_CASE("an attached mapping outlives the producer ring", "[shm]")
{
    std::unique_ptr<RingBuffer> rb(new RingBuffer(shmConfig()));
    REQUIRE(configureSmall(*rb));
    REQUIRE(writePainted(*rb));
    REQUIRE(writePainted(*rb));

    std::shared_ptr<RingBuffer> view = RingBuffer::attach(rb->sharedFd());
    REQUIRE(view);
    RingBuffer::ReadHandle held;
    REQUIRE(view->readLatest(held));

    rb.reset();   // producer gone: its mapping and its fd are released

    CHECK(intact(held.data(), held.nbytes(), 1));
    CHECK(held.stillValid());
    RingBuffer::ReadHandle again;
    REQUIRE(view->readSlot(0, again));
    CHECK(intact(again.data(), again.nbytes(), 0));
    CHECK_FALSE(view->waitForFrame(1, 20));   // nobody writes any more: plain timeout
}

TEST_CASE("attach refuses anything that is not a sealed ring of exactly the right size", "[shm]")
{
    RingBuffer rb(shmConfig());
    REQUIRE(configureSmall(rb));
    REQUIRE(writePainted(rb));

    CHECK(RingBuffer::attach(-1) == nullptr);

    SECTION("an unmodified copy is accepted")
    {
        CHECK(attachForged(rb, [](std::vector<uint8_t>&) {}) != nullptr);
    }
    SECTION("not sealed")
    {
        CHECK(attachForged(rb, [](std::vector<uint8_t>&) {}, false) == nullptr);
    }
    SECTION("bad magic")
    {
        CHECK(attachForged(rb, [](std::vector<uint8_t>& b) { headerIn(b).magic[0] = 'X'; }) == nullptr);
    }
    SECTION("other layout version")
    {
        CHECK(attachForged(rb, [](std::vector<uint8_t>& b) { headerIn(b).layoutVersion = 1; }) == nullptr);
    }
    SECTION("geometry larger than the region")
    {
        CHECK(attachForged(rb, [](std::vector<uint8_t>& b) { headerIn(b).height *= 2; }) == nullptr);
    }
    SECTION("frame size that does not match the geometry")
    {
        CHECK(attachForged(rb, [](std::vector<uint8_t>& b) { headerIn(b).frameBytes += 64; }) == nullptr);
    }
    SECTION("implausible dimensions and slot counts")
    {
        CHECK(attachForged(rb, [](std::vector<uint8_t>& b) { headerIn(b).width = 0; }) == nullptr);
        CHECK(attachForged(rb, [](std::vector<uint8_t>& b) { headerIn(b).channels = 1u << 20; }) == nullptr);
        CHECK(attachForged(rb, [](std::vector<uint8_t>& b) { headerIn(b).depth = 3; }) == nullptr);
        CHECK(attachForged(rb, [](std::vector<uint8_t>& b) { headerIn(b).slotCount = 1; }) == nullptr);
    }
    SECTION("region larger than the header says")
    {
        CHECK(attachForged(rb, [](std::vector<uint8_t>& b) { b.resize(b.size() + 4096); }) == nullptr);
    }
    SECTION("region too small for a header")
    {
        CHECK(attachForged(rb, [](std::vector<uint8_t>& b) { b.resize(100); }) == nullptr);
    }
}

TEST_CASE("a producer scribbling over the shared region cannot widen the reader's view", "[shm]")
{
    RingBuffer rb(shmConfig(4));
    REQUIRE(configureSmall(rb));
    REQUIRE(writePainted(rb));
    std::shared_ptr<RingBuffer> view = RingBuffer::attach(rb.sharedFd());
    REQUIRE(view);

    // A second mapping stands in for a broken producer process.
    const size_t size = static_cast<size_t>(rb.regionBytes());
    void* raw = ::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, rb.sharedFd(), 0);
    REQUIRE(raw != MAP_FAILED);
    auto* hdr = static_cast<hik::UC2_SessionHeader*>(raw);
    hdr->width      = 60000;
    hdr->height     = 60000;
    hdr->frameBytes = ~0ull;
    hdr->slotStride = ~0ull;
    std::memset(hdr->cameraId, 'A', sizeof(hdr->cameraId));   // no terminating NUL any more
    std::memset(hdr->comment, 'B', sizeof(hdr->comment));
    auto* slots = reinterpret_cast<hik::UC2_SlotRecord*>(
        static_cast<uint8_t*>(raw) + sizeof(hik::UC2_SessionHeader) + sizeof(hik::UC2_Control));
    slots[0].nbytes.store(~0ull);
    slots[0].payloadOffset = ~0ull;

    const hik::UC2_SessionHeader seen = view->sessionHeader();
    CHECK(seen.width == kW);
    CHECK(seen.height == kH);
    CHECK(seen.frameBytes == rb.frameBytes());
    CHECK(seen.slotStride == rb.sessionHeader().slotStride);
    CHECK(std::strlen(seen.cameraId) == sizeof(seen.cameraId) - 1);
    CHECK(std::strlen(seen.comment) == sizeof(seen.comment) - 1);
    CHECK(view->frameBytes() == rb.frameBytes());

    RingBuffer::ReadHandle r;
    REQUIRE(view->readSlot(0, r));
    CHECK(r.nbytes() == rb.frameBytes());   // clamped to the validated frame size
    CHECK(intact(r.data(), r.nbytes(), 0)); // and read from the computed address
    ::munmap(raw, size);
}

TEST_CASE("waitForFrame on an attached ring wakes when the producer commits", "[shm]")
{
    RingBuffer rb(shmConfig());
    REQUIRE(configureSmall(rb));
    std::shared_ptr<RingBuffer> view = RingBuffer::attach(rb.sharedFd());
    REQUIRE(view);

    std::thread producer([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        writePainted(rb);
    });
    const auto t0 = std::chrono::steady_clock::now();
    const bool woke = view->waitForFrame(-1, 5000);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
    producer.join();
    CHECK(woke);
    CHECK(ms < 2000);   // woken by the commit, not by the timeout
    CHECK_FALSE(view->waitForFrame(0, 0));
}

// ---------------------------------------------------------------------------
// Across processes
// ---------------------------------------------------------------------------

TEST_CASE("a frame from another process that still validates is never torn", "[shm][mt]")
{
    Child child = forkProducer(3000, 50, 0);
    const int fd = recvFd(child.sock);
    REQUIRE(fd >= 0);
    std::shared_ptr<RingBuffer> view = RingBuffer::attach(fd);
    ::close(fd);
    REQUIRE(view);

    uint64_t consistent = 0;
    uint64_t corrupt    = 0;   // must stay zero
    int64_t  last       = -1;
    bool     childDone  = false;
    while (!childDone || view->framesWritten() > static_cast<uint64_t>(last + 1))
    {
        if (!childDone && ::waitpid(child.pid, nullptr, WNOHANG) == child.pid)
        {
            childDone = true;
        }
        if (!view->waitForFrame(last, 200))
        {
            continue;
        }
        RingBuffer::ReadHandle r;
        if (!view->readLatest(r))
        {
            continue;
        }
        const bool ok = intact(r.data(), r.nbytes(), r.sequence());
        if (r.stillValid())
        {
            ok ? ++consistent : ++corrupt;
        }
        last = static_cast<int64_t>(r.sequence());
    }
    ::close(child.sock);

    CHECK(corrupt == 0);
    CHECK(consistent > 0);
    CHECK(view->framesWritten() == 3000);
}

TEST_CASE("waitForFrame wakes up when a frame is committed in another process", "[shm][mt]")
{
    Child child = forkProducer(1, 200, 0);
    const int fd = recvFd(child.sock);
    REQUIRE(fd >= 0);
    std::shared_ptr<RingBuffer> view = RingBuffer::attach(fd);
    ::close(fd);
    REQUIRE(view);

    const auto t0 = std::chrono::steady_clock::now();
    const bool woke = view->waitForFrame(-1, 10000);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
    CHECK(woke);
    CHECK(ms < 5000);   // the futex wake crossed the process boundary
    CHECK(reap(child.pid) == 0);
    ::close(child.sock);
}

TEST_CASE("a reader survives its producer process being killed mid-frame", "[shm][mt]")
{
    Child child = forkProducer(0, 0, 200);   // writes until killed
    const int fd = recvFd(child.sock);
    REQUIRE(fd >= 0);
    std::shared_ptr<RingBuffer> view = RingBuffer::attach(fd);
    ::close(fd);
    REQUIRE(view);

    REQUIRE(view->waitForFrame(10, 5000));
    RingBuffer::ReadHandle held;
    REQUIRE(view->readLatest(held));

    ::kill(child.pid, SIGKILL);
    const int status = reap(child.pid);
    CHECK(WIFSIGNALED(status));
    ::close(child.sock);

    // The pages stay mapped: every read still works, nothing raises SIGBUS.
    const uint64_t written = view->framesWritten();
    CHECK(written > 10);
    volatile uint64_t sum = 0;
    for (uint64_t i = 0; i < held.nbytes(); ++i)
    {
        sum = sum + held.data()[i];
    }
    for (uint32_t s = 0; s < view->slotCount(); ++s)
    {
        RingBuffer::ReadHandle r;
        if (view->readSlot(s, r))   // a slot killed mid-write is refused, not crashed on
        {
            CHECK(intact(r.data(), r.nbytes(), r.sequence()));
        }
    }
    CHECK_FALSE(view->waitForFrame(static_cast<int64_t>(written) - 1, 50));   // no wake-up ever comes
    CHECK(view->framesWritten() == written);
}
