#ifndef UC2HIK_CAMWORKER_H
#define UC2HIK_CAMWORKER_H

/// @file CamWorker.h
/// @brief The camera process: serves one camera to a RemoteCamera.
///
/// The program uc2_cam_worker is nothing but main() around runCameraWorker().
/// It opens the camera named in the first request, executes every further
/// request on it one after the other, and runs the acquisition and the disk
/// writer in its own threads. The ring buffer lives in a sealed memfd that is
/// handed to the application with every new session, so the application reads
/// the frames straight from shared memory.
///
/// Lifetime: the process ends after a Close request, on SIGTERM or SIGHUP, and
/// when the application's end of the socket closes -- which the kernel does
/// for it when the application process dies, however it dies. In every case
/// the camera is closed properly first, so a recording file is complete.
/// SIGINT is ignored: Ctrl+C in a terminal reaches the whole process group,
/// and closing the cameras is the application's business.

namespace hik {

/// Serve one camera over the SOCK_SEQPACKET socket @p sock until the
/// application closes it or goes away.
/// @return the process exit code (0 on an orderly end).
int runCameraWorker(int sock);

} // namespace hik

#endif // UC2HIK_CAMWORKER_H
