#include "CamIpc.h"

#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace hik {
namespace ipc {

namespace {

// Field tags. Distinct printable bytes, so a hexdump of a message is readable.
const uint8_t kTagU32  = 'u';
const uint8_t kTagI64  = 'i';
const uint8_t kTagF64  = 'd';
const uint8_t kTagFlag = 'b';
const uint8_t kTagStr  = 's';

void append(std::vector<uint8_t>& buf, const void* p, size_t n)
{
    const uint8_t* b = static_cast<const uint8_t*>(p);
    buf.insert(buf.end(), b, b + n);
}

bool knownKind(uint8_t k)
{
    return k == static_cast<uint8_t>(Kind::Request) || k == static_cast<uint8_t>(Kind::Response)
        || k == static_cast<uint8_t>(Kind::Event);
}

/// Close every descriptor in the SCM_RIGHTS data of @p msg.
void closeAllFds(struct msghdr& msg)
{
    for (struct cmsghdr* c = CMSG_FIRSTHDR(&msg); c != nullptr; c = CMSG_NXTHDR(&msg, c))
    {
        if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS)
        {
            continue;
        }
        const size_t n = (c->cmsg_len - CMSG_LEN(0)) / sizeof(int);
        for (size_t i = 0; i < n; ++i)
        {
            int fd = -1;
            std::memcpy(&fd, CMSG_DATA(c) + i * sizeof(int), sizeof(int));
            if (fd >= 0)
            {
                ::close(fd);
            }
        }
    }
}

} // namespace

// ---------------------------------------------------------------------------
// Message
// ---------------------------------------------------------------------------

Message::Message(Kind kind, Op op, uint32_t id)
    : m_kind(kind)
    , m_op(op)
    , m_id(id)
{
}

Message& Message::putU32(uint32_t v)
{
    m_buf.push_back(kTagU32);
    append(m_buf, &v, sizeof(v));
    return *this;
}

Message& Message::putI64(int64_t v)
{
    m_buf.push_back(kTagI64);
    append(m_buf, &v, sizeof(v));
    return *this;
}

Message& Message::putF64(double v)
{
    m_buf.push_back(kTagF64);
    append(m_buf, &v, sizeof(v));
    return *this;
}

Message& Message::putFlag(bool v)
{
    m_buf.push_back(kTagFlag);
    m_buf.push_back(v ? 1 : 0);
    return *this;
}

Message& Message::putStr(const std::string& v)
{
    m_buf.push_back(kTagStr);
    const uint32_t n = static_cast<uint32_t>(v.size());
    append(m_buf, &n, sizeof(n));
    append(m_buf, v.data(), v.size());
    return *this;
}

bool Message::take(uint8_t tag, size_t n, const uint8_t*& at)
{
    if (m_pos >= m_buf.size() || m_buf[m_pos] != tag || m_buf.size() - m_pos - 1 < n)
    {
        return false;
    }
    at = m_buf.data() + m_pos + 1;
    m_pos += 1 + n;
    return true;
}

bool Message::getU32(uint32_t& v)
{
    const uint8_t* at = nullptr;
    if (!take(kTagU32, sizeof(v), at))
    {
        return false;
    }
    std::memcpy(&v, at, sizeof(v));
    return true;
}

bool Message::getI64(int64_t& v)
{
    const uint8_t* at = nullptr;
    if (!take(kTagI64, sizeof(v), at))
    {
        return false;
    }
    std::memcpy(&v, at, sizeof(v));
    return true;
}

bool Message::getF64(double& v)
{
    const uint8_t* at = nullptr;
    if (!take(kTagF64, sizeof(v), at))
    {
        return false;
    }
    std::memcpy(&v, at, sizeof(v));
    return true;
}

bool Message::getFlag(bool& v)
{
    const uint8_t* at = nullptr;
    if (!take(kTagFlag, 1, at))
    {
        return false;
    }
    v = (*at != 0);
    return true;
}

bool Message::getStr(std::string& v)
{
    const size_t start = m_pos;
    const uint8_t* at = nullptr;
    uint32_t n = 0;
    if (!take(kTagStr, sizeof(n), at))
    {
        return false;
    }
    std::memcpy(&n, at, sizeof(n));
    if (m_buf.size() - m_pos < n)
    {
        m_pos = start;   // length field points past the end
        return false;
    }
    v.assign(reinterpret_cast<const char*>(m_buf.data() + m_pos), n);
    m_pos += n;
    return true;
}

std::vector<uint8_t> Message::encode() const
{
    std::vector<uint8_t> out;
    out.reserve(kHeadBytes + m_buf.size());
    const uint32_t magic   = kMagic;
    const uint16_t version = kProtocolVersion;
    const uint8_t  kind    = static_cast<uint8_t>(m_kind);
    const uint8_t  pad8    = 0;
    const uint16_t op      = static_cast<uint16_t>(m_op);
    const uint16_t pad16   = 0;
    append(out, &magic, sizeof(magic));
    append(out, &version, sizeof(version));
    append(out, &kind, sizeof(kind));
    append(out, &pad8, sizeof(pad8));
    append(out, &op, sizeof(op));
    append(out, &pad16, sizeof(pad16));
    append(out, &m_id, sizeof(m_id));
    out.insert(out.end(), m_buf.begin(), m_buf.end());
    return out;
}

bool Message::decode(const uint8_t* data, size_t n, Message& out, std::string& why)
{
    if (n < kHeadBytes)
    {
        why = "message of " + std::to_string(n) + " bytes is shorter than its head";
        return false;
    }
    uint32_t magic = 0;
    uint16_t version = 0;
    uint16_t op = 0;
    std::memcpy(&magic, data, sizeof(magic));
    std::memcpy(&version, data + 4, sizeof(version));
    const uint8_t kind = data[6];
    std::memcpy(&op, data + 8, sizeof(op));
    if (magic != kMagic)
    {
        why = "bad magic";
        return false;
    }
    if (version != kProtocolVersion)
    {
        why = "protocol version " + std::to_string(version) + ", expected "
            + std::to_string(kProtocolVersion);
        return false;
    }
    if (!knownKind(kind))
    {
        why = "unknown message kind " + std::to_string(kind);
        return false;
    }
    out.m_kind = static_cast<Kind>(kind);
    out.m_op   = static_cast<Op>(op);
    std::memcpy(&out.m_id, data + 12, sizeof(out.m_id));
    out.m_buf.assign(data + kHeadBytes, data + n);
    out.m_pos = 0;
    return true;
}

// ---------------------------------------------------------------------------
// Socket I/O
// ---------------------------------------------------------------------------

bool sendMessage(int sock, const Message& msg, int fd, bool dontWait)
{
    const std::vector<uint8_t> bytes = msg.encode();
    if (bytes.size() > kMaxMessageBytes)
    {
        errno = EMSGSIZE;
        return false;
    }
    struct iovec iov;
    iov.iov_base = const_cast<uint8_t*>(bytes.data());
    iov.iov_len  = bytes.size();

    alignas(struct cmsghdr) char ctrl[CMSG_SPACE(sizeof(int))];
    struct msghdr m;
    std::memset(&m, 0, sizeof(m));
    m.msg_iov    = &iov;
    m.msg_iovlen = 1;
    if (fd >= 0)
    {
        std::memset(ctrl, 0, sizeof(ctrl));
        m.msg_control    = ctrl;
        m.msg_controllen = sizeof(ctrl);
        struct cmsghdr* c = CMSG_FIRSTHDR(&m);
        c->cmsg_level = SOL_SOCKET;
        c->cmsg_type  = SCM_RIGHTS;
        c->cmsg_len   = CMSG_LEN(sizeof(int));
        std::memcpy(CMSG_DATA(c), &fd, sizeof(int));
    }
    const int flags = MSG_NOSIGNAL | (dontWait ? MSG_DONTWAIT : 0);
    for (;;)
    {
        const ssize_t n = ::sendmsg(sock, &m, flags);
        if (n >= 0)
        {
            return static_cast<size_t>(n) == bytes.size();
        }
        if (errno != EINTR)
        {
            return false;
        }
    }
}

RecvStatus recvMessage(int sock, Message& out, int& fd, int timeoutMs, std::string& why)
{
    fd = -1;
    if (timeoutMs >= 0)
    {
        struct pollfd p;
        p.fd      = sock;
        p.events  = POLLIN;
        p.revents = 0;
        int rc = 0;
        do
        {
            rc = ::poll(&p, 1, timeoutMs);
        } while (rc < 0 && errno == EINTR);
        if (rc == 0)
        {
            return RecvStatus::Timeout;
        }
        if (rc < 0)
        {
            why = std::string("poll: ") + std::strerror(errno);
            return RecvStatus::Error;
        }
    }

    // One byte more than any valid message: a datagram that fills it is too
    // long, whatever MSG_TRUNC says.
    std::vector<uint8_t> buf(kMaxMessageBytes + 1);
    struct iovec iov;
    iov.iov_base = buf.data();
    iov.iov_len  = buf.size();
    alignas(struct cmsghdr) char ctrl[CMSG_SPACE(4 * sizeof(int))];
    struct msghdr m;
    std::memset(&m, 0, sizeof(m));
    m.msg_iov        = &iov;
    m.msg_iovlen     = 1;
    m.msg_control    = ctrl;
    m.msg_controllen = sizeof(ctrl);

    ssize_t n = 0;
    do
    {
        n = ::recvmsg(sock, &m, MSG_CMSG_CLOEXEC);
    } while (n < 0 && errno == EINTR);
    if (n == 0)
    {
        return RecvStatus::Closed;
    }
    if (n < 0)
    {
        if (errno == ECONNRESET)
        {
            return RecvStatus::Closed;
        }
        why = std::string("recvmsg: ") + std::strerror(errno);
        return RecvStatus::Error;
    }

    const bool tooLong = (m.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) != 0
                      || static_cast<size_t>(n) > kMaxMessageBytes;
    if (tooLong || !Message::decode(buf.data(), static_cast<size_t>(n), out, why))
    {
        if (tooLong)
        {
            why = "message too long";
        }
        closeAllFds(m);
        return RecvStatus::Malformed;
    }

    // Exactly one descriptor is ever sent; keep the first, close any others.
    for (struct cmsghdr* c = CMSG_FIRSTHDR(&m); c != nullptr; c = CMSG_NXTHDR(&m, c))
    {
        if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS)
        {
            continue;
        }
        const size_t count = (c->cmsg_len - CMSG_LEN(0)) / sizeof(int);
        for (size_t i = 0; i < count; ++i)
        {
            int got = -1;
            std::memcpy(&got, CMSG_DATA(c) + i * sizeof(int), sizeof(int));
            if (fd < 0)
            {
                fd = got;
            }
            else if (got >= 0)
            {
                ::close(got);
            }
        }
    }
    return RecvStatus::Ok;
}

} // namespace ipc
} // namespace hik
