#include <cerrno>
#include <cstring>
#include <expected>
#include <fcntl.h>
#include <optional>
#include <poll.h>
#include <spawn.h>
#include <string>
#include <string_view>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

extern char **environ;

namespace skadis::process {

struct CaptureResult {
  std::string stdout_data;
  std::string stderr_data;
  int exit_code{};
};

inline std::expected<CaptureResult, std::string>
run_capture(const std::vector<std::string> &argv,
            std::optional<std::string_view> stdin_data = std::nullopt) {
  if (argv.empty()) {
    return std::unexpected("argv is empty");
  }

  int stdin_pipe[2] = {-1, -1};
  int stdout_pipe[2] = {-1, -1};
  int stderr_pipe[2] = {-1, -1};

  auto close_fd = [](int &fd) {
    if (fd >= 0) {
      ::close(fd);
      fd = -1;
    }
  };

  auto set_nonblocking = [&](int fd) -> std::expected<void, std::string> {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
      return std::unexpected(std::string("fcntl(F_GETFL) failed: ") +
                             std::strerror(errno));
    }
    if (::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
      return std::unexpected(std::string("fcntl(F_SETFL) failed: ") +
                             std::strerror(errno));
    }
    return {};
  };

  if (::pipe(stdin_pipe) < 0) {
    return std::unexpected(std::string("pipe(stdin) failed: ") +
                           std::strerror(errno));
  }
  if (::pipe(stdout_pipe) < 0) {
    int saved = errno;
    close_fd(stdin_pipe[0]);
    close_fd(stdin_pipe[1]);
    return std::unexpected(std::string("pipe(stdout) failed: ") +
                           std::strerror(saved));
  }
  if (::pipe(stderr_pipe) < 0) {
    int saved = errno;
    close_fd(stdin_pipe[0]);
    close_fd(stdin_pipe[1]);
    close_fd(stdout_pipe[0]);
    close_fd(stdout_pipe[1]);
    return std::unexpected(std::string("pipe(stderr) failed: ") +
                           std::strerror(saved));
  }

  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_adddup2(&actions, stdin_pipe[0], 0);
  posix_spawn_file_actions_adddup2(&actions, stdout_pipe[1], 1);
  posix_spawn_file_actions_adddup2(&actions, stderr_pipe[1], 2);
  posix_spawn_file_actions_addclose(&actions, stdin_pipe[0]);
  posix_spawn_file_actions_addclose(&actions, stdin_pipe[1]);
  posix_spawn_file_actions_addclose(&actions, stdout_pipe[0]);
  posix_spawn_file_actions_addclose(&actions, stdout_pipe[1]);
  posix_spawn_file_actions_addclose(&actions, stderr_pipe[0]);
  posix_spawn_file_actions_addclose(&actions, stderr_pipe[1]);

  std::vector<char *> argv_c;
  argv_c.reserve(argv.size() + 1);
  for (const auto &a : argv) {
    argv_c.push_back(const_cast<char *>(a.c_str()));
  }
  argv_c.push_back(nullptr);

  pid_t pid = -1;
  int spawn_err = ::posix_spawn(&pid, argv[0].c_str(), &actions, nullptr,
                                argv_c.data(), environ);
  ::posix_spawn_file_actions_destroy(&actions);

  close_fd(stdin_pipe[0]);
  close_fd(stdout_pipe[1]);
  close_fd(stderr_pipe[1]);

  if (spawn_err != 0) {
    close_fd(stdin_pipe[1]);
    close_fd(stdout_pipe[0]);
    close_fd(stderr_pipe[0]);
    return std::unexpected(std::string("posix_spawn failed for ") + argv[0] +
                           ": " + std::strerror(spawn_err));
  }

  if (auto result = set_nonblocking(stdin_pipe[1]); !result) {
    close_fd(stdin_pipe[1]);
    close_fd(stdout_pipe[0]);
    close_fd(stderr_pipe[0]);
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    return std::unexpected(result.error());
  }
  if (auto result = set_nonblocking(stdout_pipe[0]); !result) {
    close_fd(stdin_pipe[1]);
    close_fd(stdout_pipe[0]);
    close_fd(stderr_pipe[0]);
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    return std::unexpected(result.error());
  }
  if (auto result = set_nonblocking(stderr_pipe[0]); !result) {
    close_fd(stdin_pipe[1]);
    close_fd(stdout_pipe[0]);
    close_fd(stderr_pipe[0]);
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    return std::unexpected(result.error());
  }

  const std::string_view input = stdin_data ? *stdin_data : std::string_view();
  size_t input_offset = 0;
  if (input.empty()) {
    close_fd(stdin_pipe[1]);
  }

  std::string out;
  std::string err;
  std::optional<std::string> runtime_error;

  auto drain_fd = [&](int &fd, std::string &buffer) {
    while (fd >= 0) {
      char buf[4096];
      ssize_t n = ::read(fd, buf, sizeof(buf));
      if (n > 0) {
        buffer.append(buf, static_cast<size_t>(n));
        continue;
      }
      if (n == 0) {
        close_fd(fd);
        break;
      }
      if (errno == EINTR) {
        continue;
      }
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        break;
      }
      runtime_error = std::string("read failed: ") + std::strerror(errno);
      close_fd(fd);
      break;
    }
  };

  auto write_stdin = [&]() {
    while (stdin_pipe[1] >= 0 && input_offset < input.size()) {
      const size_t remaining = input.size() - input_offset;
      const size_t chunk = remaining > 65536 ? 65536 : remaining;
      ssize_t written =
          ::write(stdin_pipe[1], input.data() + input_offset, chunk);
      if (written > 0) {
        input_offset += static_cast<size_t>(written);
        continue;
      }
      if (written < 0 && errno == EINTR) {
        continue;
      }
      if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        break;
      }
      if (written < 0 && errno == EPIPE) {
        close_fd(stdin_pipe[1]);
        return;
      }
      runtime_error = std::string("write failed: ") + std::strerror(errno);
      close_fd(stdin_pipe[1]);
      return;
    }

    if (stdin_pipe[1] >= 0 && input_offset == input.size()) {
      close_fd(stdin_pipe[1]);
    }
  };

  while (stdin_pipe[1] >= 0 || stdout_pipe[0] >= 0 || stderr_pipe[0] >= 0) {
    std::vector<pollfd> fds;
    if (stdin_pipe[1] >= 0) {
      fds.push_back({stdin_pipe[1], POLLOUT | POLLHUP | POLLERR, 0});
    }
    if (stdout_pipe[0] >= 0) {
      fds.push_back({stdout_pipe[0], POLLIN | POLLHUP | POLLERR, 0});
    }
    if (stderr_pipe[0] >= 0) {
      fds.push_back({stderr_pipe[0], POLLIN | POLLHUP | POLLERR, 0});
    }

    int r = ::poll(fds.data(), static_cast<nfds_t>(fds.size()), -1);
    if (r < 0) {
      if (errno == EINTR) {
        continue;
      }
      runtime_error = std::string("poll failed: ") + std::strerror(errno);
      break;
    }

    size_t index = 0;
    if (stdin_pipe[1] >= 0) {
      const short events = fds[index++].revents;
      if (events & POLLOUT) {
        write_stdin();
      }
      if (stdin_pipe[1] >= 0 && (events & (POLLHUP | POLLERR))) {
        close_fd(stdin_pipe[1]);
      }
    }
    if (stdout_pipe[0] >= 0) {
      const short events = fds[index++].revents;
      if (events & (POLLIN | POLLHUP | POLLERR)) {
        drain_fd(stdout_pipe[0], out);
      }
    }
    if (stderr_pipe[0] >= 0) {
      const short events = fds[index++].revents;
      if (events & (POLLIN | POLLHUP | POLLERR)) {
        drain_fd(stderr_pipe[0], err);
      }
    }

    if (runtime_error) {
      break;
    }
  }

  close_fd(stdin_pipe[1]);
  close_fd(stdout_pipe[0]);
  close_fd(stderr_pipe[0]);

  int status = 0;
  while (::waitpid(pid, &status, 0) < 0) {
    if (errno != EINTR) {
      break;
    }
  }

  if (runtime_error) {
    return std::unexpected(*runtime_error);
  }

  int exit_code = WIFEXITED(status)     ? WEXITSTATUS(status)
                  : WIFSIGNALED(status) ? 128 + WTERMSIG(status)
                                        : -1;

  return CaptureResult{
      .stdout_data = std::move(out),
      .stderr_data = std::move(err),
      .exit_code = exit_code,
  };
}

inline std::expected<std::string, std::string>
run_capture_stdout(const std::vector<std::string> &argv,
                   std::optional<std::string_view> stdin_data = std::nullopt) {
  auto result = run_capture(argv, stdin_data);
  if (!result) {
    return std::unexpected(result.error());
  }
  if (result->exit_code != 0) {
    std::string command;
    for (const auto &a : argv) {
      if (!command.empty()) {
        command.push_back(' ');
      }
      command += a;
    }
    return std::unexpected("Command failed (exit " +
                           std::to_string(result->exit_code) + "): " + command +
                           "\n" + result->stderr_data);
  }
  return std::move(result->stdout_data);
}

} // namespace skadis::process
