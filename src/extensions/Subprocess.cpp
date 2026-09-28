// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include "extensions/Subprocess.h"

#include <fcntl.h>
#include <poll.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstddef>

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
