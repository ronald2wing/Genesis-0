// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include "extensions/Subprocess.h"

#if defined(_WIN32)

// Windows launches the child through Boost.Process v2 (plus Boost.Asio pipes)
// because there is no fork()/poll() here. The POSIX branch below keeps the
// fork/poll path for Linux/macOS. This branch is compile-unverified until a
// Windows CI leg exists; treat its semantics as best-effort and mirror the
// POSIX contract where the platform allows it.
#  include <boost/asio/io_context.hpp>
#  include <boost/asio/read.hpp>
#  include <boost/asio/readable_pipe.hpp>
#  include <boost/process/v2/process.hpp>
#  include <boost/process/v2/stdio.hpp>

#  include <string>
#  include <thread>
#  include <vector>

namespace genesis::extensions {

SubprocessResult run_subprocess(const std::vector<std::string> &argv)
{
    SubprocessResult result;
    if (argv.empty()) {
        return result; // nothing to run: started stays false
    }

    // argv[0] is the program; the constructor takes a range for the remaining
    // arguments, none of which pass through a shell. A launch failure (for
    // example a missing executable) is a Boost.Process system_error caught
    // below and reported as started=false, matching the POSIX 127 contract of
    // "reported as not available".
    std::vector<std::string> args(argv.begin() + 1, argv.end());

    boost::asio::io_context ctx;
    boost::asio::readable_pipe out_pipe{ ctx };
    boost::asio::readable_pipe err_pipe{ ctx };

    boost::process::v2::process child;
    try {
        child = boost::process::v2::process(
                ctx, argv[0], args, boost::process::v2::process_stdio{ { }, out_pipe, err_pipe });
    } catch (const std::exception &) {
        return result; // launch failed: started stays false
    }

    result.started = true;

    // The write ends now belong to the child; these readable ends expose the
    // child's stdout/stderr. Drain both from two threads so neither pipe can
    // fill and block the child while the other is still being read (the
    // classic two-pipe deadlock). Exceptions here are ignored: a truncated
    // pipe on abnormal child termination is not a launch failure, and
    // run_subprocess must not throw.
    std::thread out_reader([&] {
        try {
            boost::asio::read(out_pipe, boost::asio::dynamic_buffer(result.stdout_text));
        } catch (const std::exception &) {
            // A failed read means the child died before closing cleanly; the
            // partial capture is still returned and the process status below
            // reports the abnormal exit.
        }
    });
    std::thread err_reader([&] {
        try {
            boost::asio::read(err_pipe, boost::asio::dynamic_buffer(result.stderr_text));
        } catch (const std::exception &) {
            // Same as the stdout reader: partial capture wins over throwing.
        }
    });

    try {
        result.exit_status = child.wait();
        result.exited = true;
    } catch (const std::exception &) {
        // wait() failed: leave exited=false so the caller can tell the child
        // did not report a normal exit status.
    }

    // Join the readers. If the child is gone they hit EOF and return; the
    // readable-pipe read of a dead child's pipe is safe to abandon on the way
    // out because the process object (and with it the handles) is being
    // destroyed, not reused.
    out_reader.join();
    err_reader.join();
    return result;
}

} // namespace genesis::extensions

#else

#  include <fcntl.h>
#  include <poll.h>
#  include <sys/types.h>
#  include <sys/wait.h>
#  include <unistd.h>

#  include <array>
#  include <cerrno>
#  include <cstddef>

namespace genesis::extensions {

SubprocessResult run_subprocess(const std::vector<std::string> &argv)
{
    SubprocessResult result;
    if (argv.empty()) {
        return result; // nothing to run: started stays false
    }

    int out_pipe[2];
    int err_pipe[2];
    if (::pipe(out_pipe) != 0 || ::pipe(err_pipe) != 0) {
        return result;
    }

    const pid_t pid = ::fork();
    if (pid < 0) {
        ::close(out_pipe[0]);
        ::close(out_pipe[1]);
        ::close(err_pipe[0]);
        ::close(err_pipe[1]);
        return result;
    }

    if (pid == 0) {
        // Child: point stdout/stderr at the pipe write ends, drop the read
        // ends, and exec. execvp searches PATH; a failure is a 127 exit so the
        // parent can report "not available" rather than a generic failure.
        ::dup2(out_pipe[1], STDOUT_FILENO);
        ::dup2(err_pipe[1], STDERR_FILENO);
        ::close(out_pipe[0]);
        ::close(out_pipe[1]);
        ::close(err_pipe[0]);
        ::close(err_pipe[1]);

        std::vector<char *> c_argv;
        c_argv.reserve(argv.size() + 1);
        for (const std::string &arg : argv) {
            c_argv.push_back(const_cast<char *>(arg.c_str()));
        }
        c_argv.push_back(nullptr);
        ::execvp(c_argv[0], c_argv.data());
        ::_exit(127);
    }

    result.started = true;

    // Parent: close the write ends, then drain both read ends concurrently. A
    // naive sequential read (stdout to EOF, then stderr) would deadlock once
    // the child fills the not-yet-read pipe, so both ends are made
    // non-blocking and multiplexed over poll() until both report EOF.
    ::close(out_pipe[1]);
    ::close(err_pipe[1]);

    const int out_flags = ::fcntl(out_pipe[0], F_GETFL);
    ::fcntl(out_pipe[0], F_SETFL, out_flags | O_NONBLOCK);
    const int err_flags = ::fcntl(err_pipe[0], F_GETFL);
    ::fcntl(err_pipe[0], F_SETFL, err_flags | O_NONBLOCK);

    struct pollfd fds[2] = {
        { out_pipe[0], POLLIN, 0 },
        { err_pipe[0], POLLIN, 0 },
    };
    bool out_open = true;
    bool err_open = true;
    while (out_open || err_open) {
        const int ready = ::poll(fds, 2, -1);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        for (int i = 0; i < 2; ++i) {
            if (fds[i].fd < 0 || !(fds[i].revents & (POLLIN | POLLHUP | POLLERR))) {
                continue;
            }
            std::array<char, 4096> buffer{ };
            const ssize_t n = ::read(fds[i].fd, buffer.data(), buffer.size());
            if (n > 0) {
                (i == 0 ? result.stdout_text : result.stderr_text)
                        .append(buffer.data(), static_cast<std::size_t>(n));
                continue;
            }
            if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
                ::close(fds[i].fd);
                fds[i].fd = -1;
                (i == 0 ? out_open : err_open) = false;
            }
        }
    }

    int status = 0;
    while (::waitpid(pid, &status, 0) < 0) {
        if (errno == EINTR) {
            continue;
        }
        break;
    }
    if (WIFEXITED(status)) {
        result.exited = true;
        result.exit_status = WEXITSTATUS(status);
    }
    return result;
}

} // namespace genesis::extensions

#endif
