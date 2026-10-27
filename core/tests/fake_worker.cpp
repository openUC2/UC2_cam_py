// A stand-in for uc2_cam_worker that says hello with the wrong software
// version, for the version check of RemoteCamera (test_worker.cpp).
//
// Like the real camera process it gets its socket on file descriptor 3.
// Controlled by the environment:
//   UC2_FAKE_WORKER_VERSION   version to report; unset: a Hello without one
//   UC2_FAKE_WORKER_PIDFILE   file to write the own pid to, so the test can
//                             check that the process is gone afterwards
// After the Hello it waits until the other side closes the socket.

#include <sys/socket.h>
#include <unistd.h>

#include <cstdint>
#include <cstdlib>
#include <fstream>

#include "CamIpc.h"

int main()
{
    const int fd = 3;
    if (const char* pidFile = std::getenv("UC2_FAKE_WORKER_PIDFILE"))
    {
        std::ofstream(pidFile) << ::getpid() << "\n";
    }

    hik::ipc::Message hello(hik::ipc::Kind::Event, hik::ipc::Op::Hello, 0);
    hello.putU32(hik::ipc::kProtocolVersion).putI64(static_cast<int64_t>(::getpid()));
    if (const char* version = std::getenv("UC2_FAKE_WORKER_VERSION"))
    {
        hello.putStr(version);
    }
    if (!hik::ipc::sendMessage(fd, hello))
    {
        return 1;
    }

    char buf[256];
    while (::recv(fd, buf, sizeof(buf), 0) > 0)
    {
    }
    return 0;
}
