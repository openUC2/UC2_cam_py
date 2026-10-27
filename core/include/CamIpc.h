#ifndef UC2HIK_CAMIPC_H
#define UC2HIK_CAMIPC_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

/// @file CamIpc.h
/// @brief Message format between the application process (RemoteCamera) and a
///        camera process (uc2_cam_worker).
///
/// Transport is a `socketpair(AF_UNIX, SOCK_SEQPACKET)`: one message per
/// datagram, never split or merged, delivered in order, and end-of-file as soon
/// as the other process has closed its end -- which is also how either side
/// notices that the other one died. A ring buffer's memfd travels along with a
/// message as SCM_RIGHTS ancillary data.
///
/// A message is a fixed 16-byte head followed by typed fields. Every field
/// carries a one-byte tag, and every read checks tag and remaining length, so a
/// garbled or hostile message is rejected as malformed and never read past its
/// end.

namespace hik {
namespace ipc {

static const uint32_t kMagic           = 0x43324355u;   ///< "UC2C" in memory order
static const uint16_t kProtocolVersion = 2;   ///< 2: Hello carries the software version
static const size_t   kHeadBytes       = 16;
static const size_t   kMaxMessageBytes = 64 * 1024;     ///< far above the largest message (an 8 KiB comment)

/// What a message is.
enum class Kind : uint8_t
{
    Request  = 1,   ///< application -> camera process, answered by a Response with the same id
    Response = 2,   ///< camera process -> application
    Event    = 3    ///< camera process -> application, unsolicited
};

/// Operation of a Request/Response, or the type of an Event.
enum class Op : uint16_t
{
    // Events
    Hello = 1,          ///< u32 protocol version, i64 pid, str software version -- first message of the camera process
    Log,                ///< u32 level, str message
    Ring,               ///< i64 generation; the ring's memfd as SCM_RIGHTS

    // Requests (fields of the request -> fields of the response payload)
    Open = 100,         ///< str vendor, str connection, str serial, i64 index, u32 log level,
                        ///< u32 n, n x (str key, str value) -> str vendor, str model, str serial,
                        ///< str connection, i64 index
    CanSet,             ///< u32 Capability -> flag
    Range,              ///< u32 Quantity -> flag valid, f64 min, f64 max
    BinningValues,      ///< -> u32 n, n x u32
    GetValue,           ///< u32 Quantity -> flag ok, f64 value
    GetBinning,         ///< -> flag ok, u32 factor
    GetRoi,             ///< -> flag ok, i64 x, i64 y, i64 width, i64 height
    GetSensorSize,      ///< -> flag ok, i64 width, i64 height
    GetImageSize,       ///< -> flag ok, i64 width, i64 height
    PixelFormat,        ///< -> i64 PFNC code
    SetValue,           ///< u32 Quantity, f64 value -> flag
    SetAuto,            ///< u32 Quantity (exposure or gain) -> flag
    DisableFrameRateLimit, ///< -> flag
    SetBinning,         ///< u32 factor -> flag
    SetRoi,             ///< i64 x, i64 y, i64 width, i64 height -> flag
    ResetRoi,           ///< -> flag
    SetBufferConfig,    ///< u32 slots, str storage path, str comment -> (nothing)
    SetStoragePath,     ///< str path -> flag
    Start,              ///< -> flag
    Stop,               ///< -> (nothing)
    Close,              ///< -> (nothing); the camera process exits afterwards
    RecordingFile,      ///< -> str path
    FramesRejected,     ///< -> i64 count
    StreamStats,        ///< -> i64 saved, i64 dropped, flag error
    SimSpotPosition,    ///< i64 sequence -> f64 x, f64 y  (Simulated only)
    SimExposureAuto,    ///< -> flag                      (Simulated only)
    SimGainAuto         ///< -> flag                      (Simulated only)
};

/// Argument of Op::CanSet.
enum class Capability : uint32_t { Exposure = 0, Gain, FrameRate, Binning, Trigger, Roi };

/// Argument of Op::Range / GetValue / SetValue / SetAuto.
enum class Quantity : uint32_t { ExposureMs = 0, FrameRate, GainDb };

/// Leading field of every Response: 0 on success, else the C++ exception the
/// camera process ran into, so the application can rethrow the same type.
enum class ErrorKind : uint32_t { None = 0, InvalidArgument, OutOfRange, Runtime };

/// One message: head plus typed fields, built with the put*() calls and read
/// back in the same order with the get*() calls.
class Message
{
public:
    Message() = default;
    Message(Kind kind, Op op, uint32_t id);

    Kind     kind() const { return m_kind; }
    Op       op() const   { return m_op; }
    uint32_t id() const   { return m_id; }

    Message& putU32(uint32_t v);
    Message& putI64(int64_t v);
    Message& putF64(double v);
    Message& putFlag(bool v);
    Message& putStr(const std::string& v);

    // Each get*() returns false, and leaves the reader where it was, if the
    // next field has another type or is cut short.
    bool getU32(uint32_t& v);
    bool getI64(int64_t& v);
    bool getF64(double& v);
    bool getFlag(bool& v);
    bool getStr(std::string& v);

    /// @return true if every field has been read.
    bool atEnd() const { return m_pos == m_buf.size(); }

    /// @return head plus fields, ready to send.
    std::vector<uint8_t> encode() const;

    /// Parse a received datagram.
    /// @return false, with the reason in @p why, if it is not a valid message
    ///         of this protocol version.
    static bool decode(const uint8_t* data, size_t n, Message& out, std::string& why);

private:
    bool take(uint8_t tag, size_t n, const uint8_t*& at);

    Kind                 m_kind = Kind::Event;
    Op                   m_op   = Op::Hello;
    uint32_t             m_id   = 0;
    std::vector<uint8_t> m_buf;     ///< the fields
    size_t               m_pos = 0; ///< read cursor into m_buf
};

/// Send @p msg as one datagram, with @p fd attached when it is >= 0. Never
/// raises SIGPIPE; retries on EINTR.
/// @param dontWait return false instead of blocking when the socket is full.
/// @return false if the message could not be sent (errno tells why).
bool sendMessage(int sock, const Message& msg, int fd = -1, bool dontWait = false);

/// Outcome of recvMessage().
enum class RecvStatus { Ok, Timeout, Closed, Malformed, Error };

/// Receive one datagram into @p out. A file descriptor that came along is
/// returned in @p fd (close-on-exec), otherwise @p fd is -1; descriptors that
/// arrive with a malformed message are closed.
/// @param timeoutMs -1 blocks; otherwise Timeout after that many milliseconds.
/// @param why       reason for Malformed / Error.
RecvStatus recvMessage(int sock, Message& out, int& fd, int timeoutMs, std::string& why);

} // namespace ipc
} // namespace hik

#endif // UC2HIK_CAMIPC_H
