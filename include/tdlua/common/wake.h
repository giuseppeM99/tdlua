// Copyright (c) 2026 Giuseppe Marino
// SPDX-License-Identifier: BSD-3-Clause
#pragma once

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
// windows.h defines min/max as macros by default and breaks std::min/std::max
// in request_router.h, TDLib headers and cleanup_metadata.h. NOMINMAX is part
// of this header's public Windows contract.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/eventfd.h>
#endif
#endif

namespace tdlua {
namespace receiver {

#if defined(_WIN32)
// Process-wide Winsock lifetime. It is a function-local, intentionally leaked
// singleton: WakeTarget is only created on the owner thread at enableWake(), so
// nothing runs in DllMain, and static destruction cannot call WSACleanup while
// a leaked readiness hub still owns sockets.
//
// The reference count is ours alone. WSAStartup is refcounted by Winsock, so an
// host or TDLib that initialized Winsock keeps its own reference; our
// WSACleanup only removes ours.
class WinsockRuntime {
  public:
    static WinsockRuntime &instance() noexcept
    {
        static WinsockRuntime *runtime = new WinsockRuntime();
        return *runtime;
    }
    bool acquire() noexcept
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!started_) {
            WSADATA data;
            const int status = ::WSAStartup(MAKEWORD(2, 2), &data);
            error_.store(status, std::memory_order_relaxed);
            started_ = status == 0;
        }
        if (started_) ++users_;
        return started_;
    }
    void release() noexcept
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (users_ == 0) return;
        if (--users_ == 0 && started_) {
            ::WSACleanup();
            started_ = false;
        }
    }
    int error() const noexcept
    {
        return error_.load(std::memory_order_relaxed);
    }

  private:
    WinsockRuntime() = default;
    std::mutex mutex_;
    std::atomic<int> error_{0};
    int users_ = 0;
    bool started_ = false;
};
#endif

// One waitable object per owner. It is level-triggered and coalescing: the
// object only says "work may exist", never how many events or which client.
//
// Contract:
//   * state is published by the producer before ring();
//   * the consumer clears the object, then re-checks level state, then parks;
//   * clear() is owner-thread only; ring()/reliable() are callable from any
//     thread that holds the object alive;
//   * reliable() == false means the lost-wakeup guarantee no longer holds and
//     the host must poll hasWork() instead of trusting the descriptor.
//
// Platforms:
//   * Linux: eventfd, socketpair or pipe fallback;
//   * macOS/BSD: UNIX socket pair (SO_NOSIGPIPE), pipe fallback;
//   * Windows: non-blocking TCP loopback pair, WSAPoll, no inheritable handles.
class WakeTarget {
  public:
    WakeTarget() noexcept
#if defined(_WIN32)
        : winsock_acquired_(WinsockRuntime::instance().acquire())
#endif
    {
        open();
    }
    ~WakeTarget()
    {
        close();
#if defined(_WIN32)
        if (winsock_acquired_) WinsockRuntime::instance().release();
#endif
    }
    WakeTarget(const WakeTarget &) = delete;
    WakeTarget &operator=(const WakeTarget &) = delete;

    bool valid() const noexcept
    {
#if defined(_WIN32)
        return read_socket_ != INVALID_SOCKET && write_socket_ != INVALID_SOCKET;
#else
        return read_fd_ >= 0;
#endif
    }
    bool reliable() const noexcept
    {
        return valid() && reliable_.load(std::memory_order_acquire);
    }
    // POSIX descriptor accessor. On Windows it is always -1; use
    // nativeSocket()/readSocket() instead.
    int fd() const noexcept
    {
#if defined(_WIN32)
        return -1;
#else
        return read_fd_;
#endif
    }
    // Platform-neutral handle value: the POSIX fd or the Windows read SOCKET.
    // 0 means "no transport" (neither an fd nor a SOCKET is 0 in practice).
    std::uintptr_t nativeSocket() const noexcept
    {
#if defined(_WIN32)
        return static_cast<std::uintptr_t>(read_socket_);
#else
        return read_fd_ >= 0 ? static_cast<std::uintptr_t>(read_fd_) : 0;
#endif
    }
#if defined(_WIN32)
    SOCKET readSocket() const noexcept { return read_socket_; }
    SOCKET writeSocket() const noexcept { return write_socket_; }
#endif
    int error() const noexcept { return error_.load(std::memory_order_relaxed); }
    std::uint64_t rings() const noexcept
    {
        return rings_.load(std::memory_order_relaxed);
    }
    std::uint64_t writes() const noexcept
    {
        return writes_.load(std::memory_order_relaxed);
    }

    // Any thread. Never blocks (WOULDBLOCK means "already asserted"), never
    // throws.
    void ring() noexcept
    {
        rings_.fetch_add(1, std::memory_order_relaxed);
        if (!valid()) return;
        // Coalesce: only the 0->1 transition touches the kernel object.
        if (asserted_.exchange(true, std::memory_order_acq_rel)) return;
        writes_.fetch_add(1, std::memory_order_relaxed);
#if defined(TDLUA_TESTING)
        if (write_failure_.load(std::memory_order_relaxed)) {
            fail(brokenPipeCode());
            return;
        }
#endif
        for (int attempt = 0; attempt < 8; ++attempt) {
            const long written = writeOne();
            if (written >= 0) return;
            const int code = lastError();
            if (interrupted(code)) continue;
            if (wouldBlock(code)) return;
            fail(code);
            return;
        }
        fail(interruptedCode());
    }

    // Owner thread only. Drains the kernel object, then clears the coalescing
    // flag. The caller MUST re-check level state before parking: a producer that
    // observed asserted_ == true has already published its work.
    void clear() noexcept
    {
        if (!valid()) {
            asserted_.store(false, std::memory_order_release);
            return;
        }
#if defined(TDLUA_TESTING)
        if (write_failure_.load(std::memory_order_relaxed)) {
            fail(brokenPipeCode());
            return;
        }
#endif
        for (;;) {
            const long drained = readOne();
            if (drained > 0) continue;
            if (drained == 0) {
                // Peer closed: this transport can never signal again, so the
                // host must fall back to polling.
                fail(brokenPipeCode());
                return;
            }
            const int code = lastError();
            if (interrupted(code)) continue;
            if (wouldBlock(code)) break;
            fail(code);
            return;
        }
        asserted_.store(false, std::memory_order_release);
    }

    // 1 readable, 0 timeout, -1 unavailable/error. An interrupted wait is
    // reported as 0 so a caller loop simply re-checks stop and level state.
    int wait(int timeout_ms) const noexcept
    {
        // An unreliable transport must not be waited on: -1 tells the host to
        // fall back to polling hasWork().
        if (!valid() || !reliable()) return -1;
#if defined(_WIN32)
        WSAPOLLFD descriptor;
        descriptor.fd = read_socket_;
        descriptor.events = POLLRDNORM;
        descriptor.revents = 0;
        const int status = ::WSAPoll(&descriptor, 1, timeout_ms);
        if (status == SOCKET_ERROR) {
            return lastError() == WSAEINTR ? 0 : -1;
        }
        if (status > 0 &&
            (descriptor.revents & (POLLRDNORM | POLLHUP | POLLERR)))
            return 1;
        return status;
#else
        pollfd descriptor = {fd(), POLLIN, 0};
        const int status = ::poll(&descriptor, 1, timeout_ms);
        if (status < 0 && errno == EINTR) return 0;
        if (status > 0 && (descriptor.revents & (POLLIN | POLLHUP | POLLERR)))
            return 1;
        return status;
#endif
    }

#if defined(TDLUA_TESTING)
    static void setCreationFailureForTesting(bool fail) noexcept
    {
        creationFailureFlag().store(fail, std::memory_order_relaxed);
    }
    // 0 = platform default, 1 = UNIX socket pair, 2 = pipe. On Windows every
    // mode is the TCP loopback pair; the setter is kept so the shared tests
    // compile and run unchanged.
    static void setForcedModeForTesting(int mode) noexcept
    {
        forcedModeFlag().store(mode, std::memory_order_relaxed);
    }
    void setWriteFailureForTesting(bool fail) noexcept
    {
        write_failure_.store(fail, std::memory_order_relaxed);
    }
    // Partial close: the write side disappears, the read side stays open until
    // the destructor. The transport becomes unusable by contract.
    void closeWriteEndForTesting() noexcept
    {
        reliable_.store(false, std::memory_order_release);
#if defined(_WIN32)
        if (write_socket_ != INVALID_SOCKET) {
            ::closesocket(write_socket_);
            write_socket_ = INVALID_SOCKET;
        }
#else
        if (write_fd_ >= 0 && write_fd_ != read_fd_) {
            ::close(write_fd_);
            write_fd_ = -1;
        }
#endif
    }
#endif

  private:
#if defined(_WIN32)
    enum class Mode { TcpPair };
#else
    enum class Mode { Eventfd, SocketPair, Pipe };
#endif

    // Portable diagnostic for "this build has no wait transport". Avoids
    // relying on ENOSYS being defined by every Windows CRT.
    static const int kNoTransport = 1000;

    static std::atomic<bool> &creationFailureFlag() noexcept
    {
        static std::atomic<bool> flag{false};
        return flag;
    }
    static std::atomic<int> &forcedModeFlag() noexcept
    {
        static std::atomic<int> mode{0};
        return mode;
    }
    static int lastError() noexcept
    {
#if defined(_WIN32)
        return ::WSAGetLastError();
#else
        return errno;
#endif
    }
    static bool wouldBlock(int code) noexcept
    {
#if defined(_WIN32)
        return code == WSAEWOULDBLOCK;
#else
        return code == EAGAIN || code == EWOULDBLOCK;
#endif
    }
    static bool interrupted(int code) noexcept
    {
#if defined(_WIN32)
        return code == WSAEINTR;
#else
        return code == EINTR;
#endif
    }
    static int interruptedCode() noexcept
    {
#if defined(_WIN32)
        return WSAEINTR;
#else
        return EINTR;
#endif
    }
    static int brokenPipeCode() noexcept
    {
#if defined(_WIN32)
        return WSAECONNRESET;
#else
        return EPIPE;
#endif
    }

    void fail(int code) noexcept
    {
        error_.store(code, std::memory_order_relaxed);
        reliable_.store(false, std::memory_order_release);
        // Allow a later retry if the condition was temporary; a permanent
        // failure keeps reliable_ == false.
        asserted_.store(false, std::memory_order_release);
    }

    void open() noexcept
    {
#if defined(TDLUA_TESTING)
        if (creationFailureFlag().load(std::memory_order_relaxed)) {
            error_.store(kNoTransport, std::memory_order_relaxed);
            reliable_.store(false, std::memory_order_release);
            return;
        }
        const int forced_mode = forcedModeFlag().load(std::memory_order_relaxed);
#else
        const int forced_mode = 0;
#endif
#if defined(_WIN32)
        (void)forced_mode; // every mode maps to the TCP loopback pair
        openWindows();
#else
#if defined(__linux__)
        if (forced_mode == 0) {
            const int descriptor = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
            if (descriptor >= 0) {
                read_fd_ = write_fd_ = descriptor;
                mode_ = Mode::Eventfd;
                reliable_.store(true, std::memory_order_release);
                return;
            }
            error_.store(errno, std::memory_order_relaxed);
        }
#endif
        // macOS and the BSDs have no eventfd; Linux also lands here when
        // eventfd is unavailable. A non-blocking UNIX socket pair is pollable,
        // close-on-exec, and send() can suppress SIGPIPE.
        int fds[2] = {-1, -1};
        if (forced_mode != 2 && ::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0) {
            if (!configure(fds[0]) || !configure(fds[1])) {
                ::close(fds[0]);
                ::close(fds[1]);
            } else {
#if defined(SO_NOSIGPIPE)
                const int one = 1;
                (void)::setsockopt(fds[1], SOL_SOCKET, SO_NOSIGPIPE, &one,
                                   sizeof(one));
#endif
                read_fd_ = fds[0];
                write_fd_ = fds[1];
                mode_ = Mode::SocketPair;
                reliable_.store(true, std::memory_order_release);
                return;
            }
        }
        if (::pipe(fds) == 0) {
            if (!configure(fds[0]) || !configure(fds[1])) {
                ::close(fds[0]);
                ::close(fds[1]);
            } else {
                read_fd_ = fds[0];
                write_fd_ = fds[1];
                mode_ = Mode::Pipe;
                reliable_.store(true, std::memory_order_release);
                return;
            }
        }
        error_.store(errno, std::memory_order_relaxed);
        reliable_.store(false, std::memory_order_release);
#endif
    }

#if defined(_WIN32)
    void openWindows() noexcept
    {
        if (!winsock_acquired_) {
            error_.store(WinsockRuntime::instance().error(),
                         std::memory_order_relaxed);
            reliable_.store(false, std::memory_order_release);
            return;
        }
        SOCKET listener = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listener == INVALID_SOCKET) {
            fail(lastError());
            return;
        }
        sockaddr_in address;
        std::memset(&address, 0, sizeof(address));
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        int length = sizeof(address);
        if (::bind(listener, reinterpret_cast<sockaddr *>(&address),
                   static_cast<int>(sizeof(address))) != 0 ||
            ::listen(listener, 1) != 0 ||
            ::getsockname(listener, reinterpret_cast<sockaddr *>(&address),
                          &length) != 0) {
            ::closesocket(listener);
            fail(lastError());
            return;
        }
        SOCKET writer = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (writer == INVALID_SOCKET) {
            ::closesocket(listener);
            fail(lastError());
            return;
        }
        // Loopback connect to a listening socket completes immediately.
        if (::connect(writer, reinterpret_cast<sockaddr *>(&address),
                      static_cast<int>(sizeof(address))) != 0) {
            ::closesocket(writer);
            ::closesocket(listener);
            fail(lastError());
            return;
        }
        SOCKET reader = ::accept(listener, nullptr, nullptr);
        ::closesocket(listener);
        if (reader == INVALID_SOCKET) {
            ::closesocket(writer);
            fail(lastError());
            return;
        }
        if (!configure(reader) || !configure(writer)) {
            const int code = lastError();
            ::closesocket(reader);
            ::closesocket(writer);
            fail(code);
            return;
        }
        read_socket_ = reader;
        write_socket_ = writer;
        mode_ = Mode::TcpPair;
        reliable_.store(true, std::memory_order_release);
    }
    static bool configure(SOCKET socket) noexcept
    {
        u_long nonblocking = 1;
        if (::ioctlsocket(socket, FIONBIO, &nonblocking) != 0) return false;
        if (!::SetHandleInformation(reinterpret_cast<HANDLE>(socket),
                                    HANDLE_FLAG_INHERIT, 0))
            return false;
        return true;
    }
    long writeOne() noexcept
    {
        const char byte = 1;
        return static_cast<long>(::send(write_socket_, &byte, 1, 0));
    }
    long readOne() noexcept
    {
        char buffer[64];
        return static_cast<long>(::recv(read_socket_, buffer, sizeof(buffer), 0));
    }
#else
    static bool configure(int descriptor) noexcept
    {
        const int flags = ::fcntl(descriptor, F_GETFL, 0);
        if (flags < 0 ||
            ::fcntl(descriptor, F_SETFL, flags | O_NONBLOCK) < 0)
            return false;
        const int descriptor_flags = ::fcntl(descriptor, F_GETFD, 0);
        if (descriptor_flags < 0 ||
            ::fcntl(descriptor, F_SETFD, descriptor_flags | FD_CLOEXEC) < 0)
            return false;
        return true;
    }
    long writeOne() noexcept
    {
        if (mode_ == Mode::Eventfd) {
            const std::uint64_t one = 1;
            return static_cast<long>(::write(write_fd_, &one, sizeof(one)));
        }
        const char byte = 1;
        if (mode_ == Mode::SocketPair) {
#if defined(MSG_NOSIGNAL)
            return static_cast<long>(::send(write_fd_, &byte, 1, MSG_NOSIGNAL));
#else
            return static_cast<long>(::send(write_fd_, &byte, 1, 0));
#endif
        }
        return static_cast<long>(::write(write_fd_, &byte, 1));
    }
    // > 0: drained some bytes; 0: write side closed; -1: errno set.
    long readOne() noexcept
    {
        if (mode_ == Mode::Eventfd) {
            std::uint64_t value = 0;
            return static_cast<long>(::read(read_fd_, &value, sizeof(value)));
        }
        char buffer[64];
        return static_cast<long>(::read(read_fd_, buffer, sizeof(buffer)));
    }
#endif

    void close() noexcept
    {
#if defined(_WIN32)
        if (read_socket_ != INVALID_SOCKET) ::closesocket(read_socket_);
        if (write_socket_ != INVALID_SOCKET) ::closesocket(write_socket_);
        read_socket_ = INVALID_SOCKET;
        write_socket_ = INVALID_SOCKET;
#else
        if (read_fd_ >= 0) {
            if (write_fd_ >= 0 && write_fd_ != read_fd_) ::close(write_fd_);
            ::close(read_fd_);
        } else if (write_fd_ >= 0) {
            ::close(write_fd_);
        }
        read_fd_ = write_fd_ = -1;
#endif
    }

#if defined(_WIN32)
    Mode mode_ = Mode::TcpPair;
    SOCKET read_socket_ = INVALID_SOCKET;
    SOCKET write_socket_ = INVALID_SOCKET;
    bool winsock_acquired_ = false;
#else
    Mode mode_ = Mode::SocketPair;
    int read_fd_ = -1;
    int write_fd_ = -1;
#endif
    std::atomic<bool> asserted_{false};
    std::atomic<bool> reliable_{false};
    std::atomic<int> error_{0};
    std::atomic<std::uint64_t> rings_{0}, writes_{0};
#if defined(TDLUA_TESTING)
    std::atomic<bool> write_failure_{false};
#endif
};

} // namespace receiver
} // namespace tdlua
