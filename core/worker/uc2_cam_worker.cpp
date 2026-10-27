// uc2_cam_worker - camera process of uc2_cam_py.
//
// Started by RemoteCamera (one process per camera) with its end of a
// SOCK_SEQPACKET socket pair on file descriptor 3. Not meant to be run by hand;
// everything it does is described in CamWorker.h.

#include <sys/socket.h>
#include <sys/stat.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "CamWorker.h"

int main(int argc, char** argv)
{
    int fd = 3;
    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--fd") == 0 && i + 1 < argc)
        {
            fd = std::atoi(argv[++i]);
        }
        else
        {
            std::fprintf(stderr, "usage: %s [--fd N]   (started by uc2_cam_py, not by hand)\n", argv[0]);
            return 2;
        }
    }

    struct stat st;
    int type = 0;
    socklen_t len = sizeof(type);
    if (::fstat(fd, &st) != 0 || !S_ISSOCK(st.st_mode)
        || ::getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &len) != 0 || type != SOCK_SEQPACKET)
    {
        std::fprintf(stderr, "uc2_cam_worker: file descriptor %d is not the socket of a uc2_cam_py "
                             "application - this program is started by uc2_cam_py, not by hand\n", fd);
        return 2;
    }
    return hik::runCameraWorker(fd);
}
