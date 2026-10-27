// Message format between the application and a camera process. The property
// under test: whatever arrives on the socket, the reader never reads past a
// message and never keeps a descriptor it did not ask for.
#include <catch2/catch_test_macros.hpp>

#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>
#include <string>
#include <vector>

#include "CamIpc.h"

using hik::ipc::Kind;
using hik::ipc::Message;
using hik::ipc::Op;
using hik::ipc::RecvStatus;

namespace {

struct SocketPair
{
    int a = -1;
    int b = -1;
    SocketPair()
    {
        int sv[2];
        REQUIRE(::socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv) == 0);
        a = sv[0];
        b = sv[1];
    }
    ~SocketPair()
    {
        if (a >= 0) { ::close(a); }
        if (b >= 0) { ::close(b); }
    }
};

bool fdIsOpen(int fd)
{
    return ::fcntl(fd, F_GETFD) != -1;
}

} // namespace

TEST_CASE("a message round-trips all field types in order", "[ipc]")
{
    Message m(Kind::Request, Op::SetRoi, 42);
    m.putU32(7).putI64(-5).putF64(2.5).putFlag(true).putStr("Mono16").putStr("");
    const std::vector<uint8_t> bytes = m.encode();

    Message back;
    std::string why;
    REQUIRE(Message::decode(bytes.data(), bytes.size(), back, why));
    CHECK(back.kind() == Kind::Request);
    CHECK(back.op() == Op::SetRoi);
    CHECK(back.id() == 42);

    uint32_t u = 0;
    int64_t i = 0;
    double d = 0;
    bool b = false;
    std::string s1, s2;
    CHECK(back.getU32(u));
    CHECK(back.getI64(i));
    CHECK(back.getF64(d));
    CHECK(back.getFlag(b));
    CHECK(back.getStr(s1));
    CHECK(back.getStr(s2));
    CHECK(back.atEnd());
    CHECK(u == 7);
    CHECK(i == -5);
    CHECK(d == 2.5);
    CHECK(b);
    CHECK(s1 == "Mono16");
    CHECK(s2.empty());
}

TEST_CASE("reading a field of the wrong type or past the end fails without moving on", "[ipc]")
{
    Message m(Kind::Response, Op::GetRoi, 1);
    m.putI64(3);
    const std::vector<uint8_t> bytes = m.encode();
    Message back;
    std::string why;
    REQUIRE(Message::decode(bytes.data(), bytes.size(), back, why));

    uint32_t u = 0;
    std::string s;
    CHECK_FALSE(back.getU32(u));   // it is an i64
    CHECK_FALSE(back.getStr(s));
    int64_t i = 0;
    CHECK(back.getI64(i));         // the failed reads did not consume it
    CHECK(i == 3);
    CHECK_FALSE(back.getI64(i));   // nothing left
}

TEST_CASE("a string whose length points past the message is rejected", "[ipc]")
{
    Message m(Kind::Event, Op::Log, 0);
    m.putStr("hello");
    std::vector<uint8_t> bytes = m.encode();
    const uint32_t huge = 0xFFFFFFF0u;
    std::memcpy(bytes.data() + hik::ipc::kHeadBytes + 1, &huge, sizeof(huge));

    Message back;
    std::string why;
    REQUIRE(Message::decode(bytes.data(), bytes.size(), back, why));
    std::string s;
    CHECK_FALSE(back.getStr(s));
    CHECK(s.empty());
}

TEST_CASE("decode rejects foreign and broken heads", "[ipc]")
{
    const std::vector<uint8_t> good = Message(Kind::Request, Op::Start, 1).encode();
    Message out;
    std::string why;

    CHECK_FALSE(Message::decode(good.data(), 10, out, why));   // shorter than the head

    std::vector<uint8_t> bad = good;
    bad[0] ^= 0xFF;
    CHECK_FALSE(Message::decode(bad.data(), bad.size(), out, why));
    CHECK(why == "bad magic");

    bad = good;
    bad[4] = 99;   // protocol version
    CHECK_FALSE(Message::decode(bad.data(), bad.size(), out, why));

    bad = good;
    bad[6] = 77;   // kind
    CHECK_FALSE(Message::decode(bad.data(), bad.size(), out, why));
}

TEST_CASE("recvMessage delivers a message with its descriptor", "[ipc]")
{
    SocketPair sp;
    const int fd = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
    REQUIRE(fd >= 0);
    Message m(Kind::Event, Op::Ring, 0);
    m.putI64(3);
    REQUIRE(hik::ipc::sendMessage(sp.a, m, fd));
    ::close(fd);

    Message got;
    int gotFd = -1;
    std::string why;
    REQUIRE(hik::ipc::recvMessage(sp.b, got, gotFd, 1000, why) == RecvStatus::Ok);
    CHECK(got.op() == Op::Ring);
    REQUIRE(gotFd >= 0);
    CHECK((::fcntl(gotFd, F_GETFD) & FD_CLOEXEC) != 0);
    ::close(gotFd);
}

TEST_CASE("recvMessage reports timeouts, end-of-file and garbage", "[ipc]")
{
    SocketPair sp;
    Message got;
    int fd = -1;
    std::string why;

    CHECK(hik::ipc::recvMessage(sp.b, got, fd, 20, why) == RecvStatus::Timeout);

    // Garbage with a descriptor attached: rejected, and the descriptor closed.
    const char junk[] = "definitely not a uc2 message";
    const int devnull = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
    REQUIRE(devnull >= 0);
    {
        struct iovec iov;
        iov.iov_base = const_cast<char*>(junk);
        iov.iov_len  = sizeof(junk);
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
        std::memcpy(CMSG_DATA(c), &devnull, sizeof(int));
        REQUIRE(::sendmsg(sp.a, &msg, MSG_NOSIGNAL) == static_cast<ssize_t>(sizeof(junk)));
    }
    ::close(devnull);
    const int before = ::open("/dev/null", O_RDONLY | O_CLOEXEC);   // lowest free number now
    ::close(before);
    CHECK(hik::ipc::recvMessage(sp.b, got, fd, 1000, why) == RecvStatus::Malformed);
    CHECK(fd == -1);
    CHECK_FALSE(fdIsOpen(before));   // the received copy was closed again

    // Too long for the protocol: rejected as a whole.
    std::vector<uint8_t> big(hik::ipc::kMaxMessageBytes + 100, 0);
    const std::vector<uint8_t> head = Message(Kind::Event, Op::Log, 0).encode();
    std::memcpy(big.data(), head.data(), head.size());
    REQUIRE(::send(sp.a, big.data(), big.size(), MSG_NOSIGNAL) == static_cast<ssize_t>(big.size()));
    CHECK(hik::ipc::recvMessage(sp.b, got, fd, 1000, why) == RecvStatus::Malformed);

    ::close(sp.a);
    sp.a = -1;
    CHECK(hik::ipc::recvMessage(sp.b, got, fd, 1000, why) == RecvStatus::Closed);
}

TEST_CASE("sendMessage refuses messages above the size limit", "[ipc]")
{
    SocketPair sp;
    Message m(Kind::Event, Op::Log, 0);
    m.putStr(std::string(hik::ipc::kMaxMessageBytes, 'x'));
    CHECK_FALSE(hik::ipc::sendMessage(sp.a, m));
}
