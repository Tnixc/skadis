#include <cerrno>
#include <cstring>
#include <expected>
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

  if (stdin_data && !stdin_data->empty()) {
    const char *data = stdin_data->data();
    size_t remaining = stdin_data->size();
    while (remaining > 0) {
      ssize_t written = ::write(stdin_pipe[1], data, remaining);
      if (written < 0) {
        if (errno == EINTR) {
          continue;
        }
        break;
      }
      data += written;
      remaining -= static_cast<size_t>(written);
    }
  }
  close_fd(stdin_pipe[1]);

  std::string out;
  std::string err;
  pollfd fds[2] = {{stdout_pipe[0], POLLIN, 0}, {stderr_pipe[0], POLLIN, 0}};
  int open_count = 2;
  while (open_count > 0) {
    int r = ::poll(fds, 2, -1);
    if (r < 0) {
      if (errno == EINTR) {
        continue;
      }
      break;
    }
    for (int i = 0; i < 2; ++i) {
      if (fds[i].fd < 0) {
        continue;
      }
      if (fds[i].revents & (POLLIN | POLLHUP | POLLERR)) {
        char buf[4096];
        ssize_t n = ::read(fds[i].fd, buf, sizeof(buf));
        if (n > 0) {
          (i == 0 ? out : err).append(buf, static_cast<size_t>(n));
        } else if (n == 0 ||
                   (n < 0 && errno != EINTR && errno != EAGAIN)) {
          ::close(fds[i].fd);
          fds[i].fd = -1;
          --open_count;
        }
      }
    }
  }
  close_fd(stdout_pipe[0]);
  close_fd(stderr_pipe[0]);

  int status = 0;
  while (::waitpid(pid, &status, 0) < 0) {
    if (errno != EINTR) {
      break;
    }
  }

  int exit_code = WIFEXITED(status) ? WEXITSTATUS(status)
                  : WIFSIGNALED(status)
                      ? 128 + WTERMSIG(status)
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
                           std::to_string(result->exit_code) +
                           "): " + command + "\n" + result->stderr_data);
  }
  return std::move(result->stdout_data);
}

} // namespace skadis::process
