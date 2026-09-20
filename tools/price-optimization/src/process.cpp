// SPDX-License-Identifier: LicenseRef-Praesidium-Humanitatis-Worker-Protection-1.0
#include "ph/price/process.hpp"
#include <array>
#include <cerrno>
#include <cstdint>
#include <limits>

#ifdef __linux__
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#endif

namespace ph::price::process {
#ifdef __linux__
namespace {
constexpr std::size_t input_limit = 8 * 1024 * 1024;
constexpr std::size_t output_limit = 8 * 1024 * 1024;
constexpr std::size_t diagnostic_limit = 64 * 1024;
constexpr std::uint32_t report_magic = 0x50485052;
struct Report {
  std::uint32_t magic = report_magic;
  int startup_error = 0;
  int wait_status = -1;
  int cleanup_error = 0;
  int deadline_triggered = 0;
};

int deadline_state(const struct timespec& deadline) {
  struct timespec now {};
  if (::clock_gettime(CLOCK_MONOTONIC, &now) != 0) return -1;
  return now.tv_sec > deadline.tv_sec ||
      (now.tv_sec == deadline.tv_sec && now.tv_nsec >= deadline.tv_nsec) ? 1 : 0;
}

void close_fd(int& fd) {
  if (fd >= 0) {
    // On Linux an EINTR close has already released the descriptor. Retrying
    // could close an unrelated descriptor opened by another caller thread.
    ::close(fd);
    fd = -1;
  }
}

bool nonblocking(int fd) {
  const int flags = ::fcntl(fd, F_GETFL);
  return flags >= 0 && ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

bool above_stdio(int& fd) {
  if (fd >= 3) return true;
  const int replacement = ::fcntl(fd, F_DUPFD_CLOEXEC, 3);
  if (replacement < 0) return false;
  close_fd(fd);
  fd = replacement;
  return true;
}

// A different thread may already own another invocation's control pipe. A
// forked supervisor must not keep that unrelated pipe alive: otherwise EOF
// cancellation/parent-death detection could depend on the other solve ending.
bool close_unrelated(std::array<int, 7> keep) {
  for (std::size_t i = 0; i < keep.size(); ++i)
    for (std::size_t j = i + 1; j < keep.size(); ++j)
      if (keep[j] < keep[i]) { const int saved = keep[i]; keep[i] = keep[j]; keep[j] = saved; }
  unsigned int first = 3;
  for (const int fd : keep) {
    if (first < static_cast<unsigned int>(fd) &&
        ::syscall(SYS_close_range, first, static_cast<unsigned int>(fd) - 1, 0) != 0) return false;
    first = static_cast<unsigned int>(fd) + 1;
  }
  return ::syscall(SYS_close_range, first, std::numeric_limits<unsigned int>::max(), 0) == 0;
}

void write_fixed(int fd, const void* value, std::size_t size) {
  auto bytes = static_cast<const char*>(value);
  while (size != 0) {
    const auto count = ::write(fd, bytes, size);
    if (count > 0) {
      bytes += count;
      size -= static_cast<std::size_t>(count);
    } else if (count < 0 && errno == EINTR) {
      continue;
    } else {
      return;
    }
  }
}

// The supervisor alone adopts/reaps this invocation's descendants. It never
// changes the dashboard process's global SIGCHLD or subreaper configuration.
// This also catches a descendant that creates another process group/session.
// Each listed PID is an unreaped direct child, so it cannot be reused while
// the supervisor signals it. This function does not reap anything itself.
bool kill_adopted_children() {
  char path[96] = "/proc/self/task/";
  std::size_t used = 16;
  char digits[32];
  std::size_t count = 0;
  auto pid = static_cast<unsigned long>(::getpid());
  do {
    digits[count++] = static_cast<char>('0' + pid % 10);
    pid /= 10;
  } while (pid != 0);
  while (count != 0) path[used++] = digits[--count];
  constexpr char suffix[] = "/children";
  for (const char ch : suffix) path[used++] = ch;
  const int fd = ::open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) return false;
  char bytes[4096];
  unsigned long child = 0;
  bool okay = true;
  for (;;) {
    const auto size = ::read(fd, bytes, sizeof(bytes));
    if (size < 0 && errno == EINTR) continue;
    if (size <= 0) {
      if (size < 0) okay = false;
      break;
    }
    for (ssize_t i = 0; i < size; ++i) {
      if (bytes[i] >= '0' && bytes[i] <= '9') {
        child = child * 10 + static_cast<unsigned long>(bytes[i] - '0');
      } else if (child != 0) {
        if (::kill(static_cast<pid_t>(child), SIGKILL) != 0 && errno != ESRCH) okay = false;
        child = 0;
      }
    }
  }
  if (child != 0 && ::kill(static_cast<pid_t>(child), SIGKILL) != 0 && errno != ESRCH) okay = false;
  ::close(fd);
  return okay;
}

[[noreturn]] void supervise(const char* executable, char* const* arguments,
                            int input_fd, int output_fd, int diagnostic_fd,
                            int control_fd, int report_fd, int exec_read_fd,
                            int exec_write_fd, const struct timespec& deadline) {
  Report report;
  if (!close_unrelated({input_fd, output_fd, diagnostic_fd, control_fd,
                        report_fd, exec_read_fd, exec_write_fd})) {
    report.startup_error = errno;
    write_fixed(report_fd, &report, sizeof(report));
    ::_exit(1);
  }
  struct sigaction default_action {};
  default_action.sa_handler = SIG_DFL;
  ::sigemptyset(&default_action.sa_mask);
  // An embedding host may ignore SIGCHLD. Auto-reaping would break the
  // guarantee that the worker PID remains reserved until group cleanup.
  if (::sigaction(SIGCHLD, &default_action, nullptr) != 0 ||
      ::prctl(PR_SET_CHILD_SUBREAPER, 1, 0, 0, 0) != 0) {
    report.startup_error = errno;
    write_fixed(report_fd, &report, sizeof(report));
    ::_exit(1);
  }
  const int initial_deadline = deadline_state(deadline);
  if (initial_deadline != 0) {
    if (initial_deadline < 0) report.startup_error = errno;
    else report.deadline_triggered = 1;
    write_fixed(report_fd, &report, sizeof(report));
    ::_exit(0);
  }
  const pid_t worker = ::fork();
  if (worker < 0) {
    report.startup_error = errno;
    write_fixed(report_fd, &report, sizeof(report));
    ::_exit(1);
  }
  if (worker == 0) {
    ::close(control_fd);
    ::close(report_fd);
    ::close(exec_read_fd);
    if (::setpgid(0, 0) != 0 || ::dup2(input_fd, STDIN_FILENO) < 0 ||
        ::dup2(output_fd, STDOUT_FILENO) < 0 || ::dup2(diagnostic_fd, STDERR_FILENO) < 0) {
      const int error = errno;
      write_fixed(exec_write_fd, &error, sizeof(error));
      ::_exit(127);
    }
    ::close(input_fd);
    ::close(output_fd);
    ::close(diagnostic_fd);
    // All allocation/argv construction happened before the first fork.
    ::execv(executable, arguments);
    const int error = errno;
    write_fixed(exec_write_fd, &error, sizeof(error));
    ::_exit(127);
  }
  ::close(input_fd);
  ::close(output_fd);
  ::close(diagnostic_fd);
  ::close(exec_write_fd);
  // Close the race between fork and a parent-requested cancellation. The
  // worker also sets its own group before it can execute caller code.
  int grouped;
  do { grouped = ::setpgid(worker, worker); } while (grouped != 0 && errno == EINTR);
  if (grouped != 0 && errno != EACCES && errno != ESRCH) report.startup_error = errno;

  bool finished = report.startup_error != 0;
  while (!finished) {
    siginfo_t info {};
    if (::waitid(P_PID, static_cast<id_t>(worker), &info, WEXITED | WNOHANG | WNOWAIT) == 0) {
      finished = info.si_pid == worker;
    } else if (errno != EINTR) {
      report.cleanup_error = errno;
      finished = true;
    }
    if (finished) break;
    // Enforce the worker deadline independently of the embedding thread. A
    // slow callback cannot keep AMPL alive while parent polling is paused.
    // Do not inspect this clock during cleanup after an observed worker exit.
    const int expired = deadline_state(deadline);
    if (expired != 0) {
      if (expired < 0) report.cleanup_error = errno;
      else report.deadline_triggered = 1;
      break;
    }
    struct pollfd control {control_fd, POLLIN | POLLHUP, 0};
    const int ready = ::poll(&control, 1, 5);
    if (ready > 0) finished = true;  // Closing the parent's end requests cleanup.
    else if (ready < 0 && errno != EINTR) {
      report.cleanup_error = errno;
      finished = true;
    }
  }
  // The leader is deliberately still unreaped: its PID/group ID cannot have
  // been reused by an unrelated process. Never signal the group after reaping.
  if (::kill(-worker, SIGKILL) != 0 && errno != ESRCH) report.cleanup_error = errno;
  if (::kill(worker, SIGKILL) != 0 && errno != ESRCH) report.cleanup_error = errno;
  for (;;) {
    int status = 0;
    const pid_t reaped = ::waitpid(-1, &status, WNOHANG);
    if (reaped > 0) {
      if (reaped == worker) report.wait_status = status;
      continue;
    }
    if (reaped < 0) {
      if (errno == EINTR) continue;
      if (errno != ECHILD) report.cleanup_error = errno;
      break;
    }
    if (!kill_adopted_children()) report.cleanup_error = errno != 0 ? errno : EIO;
    // SIGKILL cannot interrupt uninterruptible kernel sleep. Cleanup waits for
    // actual reaping instead of claiming success while zombies/children remain.
    ::poll(nullptr, 0, 5);
  }
  int exec_error = 0;
  std::size_t received = 0;
  while (received < sizeof(exec_error)) {
    const auto size = ::read(exec_read_fd, reinterpret_cast<char*>(&exec_error) + received,
                             sizeof(exec_error) - received);
    if (size > 0) received += static_cast<std::size_t>(size);
    else if (size < 0 && errno == EINTR) continue;
    else break;
  }
  if (received == sizeof(exec_error)) report.startup_error = exec_error;
  ::close(exec_read_fd);
  write_fixed(report_fd, &report, sizeof(report));
  ::_exit(report.cleanup_error != 0 ? 1 : 0);
}
}  // namespace
#endif

Result run(const std::vector<std::string>& argv, const std::string& input,
           std::chrono::milliseconds timeout, const std::function<bool()>& cancelled) {
  Result result;
  const auto start = std::chrono::steady_clock::now();
  auto elapsed = [&] { return std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - start); };
#ifndef __linux__
  (void)argv; (void)input; (void)timeout; (void)cancelled;
  result.error = "bounded worker execution requires Linux";
#else
  struct timespec monotonic_start {};
  if (::clock_gettime(CLOCK_MONOTONIC, &monotonic_start) != 0) {
    result.error = "could not read the monotonic worker clock";
    return result;
  }
  if (argv.empty() || argv.front().empty() || argv.front().front() != '/') {
    result.error = "worker executable must be an absolute path";
    return result;
  }
  for (const auto& argument : argv) {
    if (argument.find('\0') != std::string::npos) {
      result.error = "worker arguments must not contain NUL bytes";
      return result;
    }
  }
  if (input.size() > input_limit || timeout.count() <= 0) {
    result.error = "worker input exceeds 8 MiB or timeout is not positive";
    return result;
  }
  struct timespec deadline = monotonic_start;
  const auto seconds = timeout.count() / 1000;
  const auto nanoseconds = (timeout.count() % 1000) * 1000000;
  if (seconds > std::numeric_limits<time_t>::max() - deadline.tv_sec - 1) {
    deadline.tv_sec = std::numeric_limits<time_t>::max();
    deadline.tv_nsec = 999999999;
  } else {
    deadline.tv_sec += static_cast<time_t>(seconds);
    deadline.tv_nsec += static_cast<long>(nanoseconds);
    if (deadline.tv_nsec >= 1000000000) {
      ++deadline.tv_sec;
      deadline.tv_nsec -= 1000000000;
    }
  }
  auto cancellation_requested = [&] {
    try { return cancelled && cancelled(); }
    catch (...) {
      result.error = "cancellation callback threw an exception";
      return true;
    }
  };
  if (cancellation_requested()) {
    if (result.error.empty()) result.status = Status::cancelled;
    result.elapsed = elapsed();
    return result;
  }

  // Reserve before fork: later bounded captures cannot allocate while the
  // invocation owns processes. The supervisor/worker use only syscall-style
  // operations before exec, with no inherited C++/allocator locks.
  result.output.reserve(output_limit);
  result.diagnostics.reserve(diagnostic_limit);
  result.error.reserve(256);
  std::vector<char*> arguments;
  arguments.reserve(argv.size() + 1);
  for (const auto& argument : argv) arguments.push_back(const_cast<char*>(argument.c_str()));
  arguments.push_back(nullptr);
  int input_pair[2] = {-1, -1};
  int output_pipe[2] = {-1, -1};
  int diagnostic_pipe[2] = {-1, -1};
  int control_pipe[2] = {-1, -1};
  int report_pipe[2] = {-1, -1};
  int exec_pipe[2] = {-1, -1};
  auto close_all = [&] {
    for (int* pair : {input_pair, output_pipe, diagnostic_pipe, control_pipe, report_pipe, exec_pipe})
      for (int i = 0; i != 2; ++i) close_fd(pair[i]);
  };
  if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, input_pair) != 0 ||
      ::pipe2(output_pipe, O_CLOEXEC) != 0 || ::pipe2(diagnostic_pipe, O_CLOEXEC) != 0 ||
      ::pipe2(control_pipe, O_CLOEXEC) != 0 || ::pipe2(report_pipe, O_CLOEXEC) != 0 ||
      ::pipe2(exec_pipe, O_CLOEXEC) != 0) {
    result.error = "could not create worker communication descriptors";
    close_all();
    result.elapsed = elapsed();
    return result;
  }
  bool descriptors_ready = true;
  for (int* pair : {input_pair, output_pipe, diagnostic_pipe, control_pipe, report_pipe, exec_pipe})
    for (int i = 0; i != 2; ++i) descriptors_ready = above_stdio(pair[i]) && descriptors_ready;
  if (!descriptors_ready || !nonblocking(input_pair[1]) || !nonblocking(output_pipe[0]) ||
      !nonblocking(diagnostic_pipe[0]) || !nonblocking(report_pipe[0])) {
    result.error = "could not configure worker communication descriptors";
    close_all();
    result.elapsed = elapsed();
    return result;
  }
  const pid_t supervisor = ::fork();
  if (supervisor == 0) {
    ::close(input_pair[1]); ::close(output_pipe[0]); ::close(diagnostic_pipe[0]);
    ::close(control_pipe[1]); ::close(report_pipe[0]);
    supervise(argv.front().c_str(), arguments.data(), input_pair[0], output_pipe[1],
              diagnostic_pipe[1], control_pipe[0], report_pipe[1], exec_pipe[0], exec_pipe[1], deadline);
  }
  close_fd(input_pair[0]); close_fd(output_pipe[1]); close_fd(diagnostic_pipe[1]);
  close_fd(control_pipe[0]); close_fd(report_pipe[1]); close_fd(exec_pipe[0]); close_fd(exec_pipe[1]);
  if (supervisor < 0) {
    result.error = "could not create worker supervisor";
    close_all();
    result.elapsed = elapsed();
    return result;
  }

  bool stopping = false;
  bool failure_before_deadline = false;
  bool truncated_diagnostics = false;
  std::size_t written = 0;
  Report report;
  std::size_t report_size = 0;
  auto stop = [&](Status status, const char* error) {
    if (!stopping) {
      result.status = status;
      failure_before_deadline = status == Status::failed && elapsed() < timeout;
      if (error != nullptr) result.error = error;
      stopping = true;
    }
    close_fd(control_pipe[1]);
    close_fd(input_pair[1]);
  };
  auto drain = [&](int& fd, std::string& target, std::size_t limit, bool diagnostic) {
    std::array<char, 16384> bytes {};
    // Bound work per iteration so a continuously writing child cannot starve
    // deadline and cancellation checks.
    for (int reads = 0; reads != 4 && fd >= 0; ++reads) {
      const auto size = ::read(fd, bytes.data(), bytes.size());
      if (size > 0) {
        const auto available = limit - target.size();
        const auto count = static_cast<std::size_t>(size);
        target.append(bytes.data(), count < available ? count : available);
        if (count > available) {
          if (diagnostic) truncated_diagnostics = true;
          else stop(Status::failed, "worker stdout exceeded 8 MiB");
        }
      } else if (size == 0) {
        close_fd(fd);
      } else if (errno == EINTR) {
        continue;
      } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
        break;
      } else {
        stop(Status::failed, "could not read worker output");
        close_fd(fd);
      }
    }
  };
  if (input.empty()) close_fd(input_pair[1]);
  while (output_pipe[0] >= 0 || diagnostic_pipe[0] >= 0 || report_pipe[0] >= 0) {
    if (!stopping) {
      if (cancellation_requested()) {
        stop(result.error.empty() ? Status::cancelled : Status::failed, nullptr);
      } else if (elapsed() >= timeout) {
        stop(Status::timed_out, "worker deadline exceeded");
      }
    }
    const auto remaining = timeout - elapsed();
    const int wait_ms = stopping ? 5 : static_cast<int>(remaining.count() < 5 ?
        (remaining.count() > 0 ? remaining.count() : 0) : 5);
    std::array<struct pollfd, 4> fds {{{input_pair[1], POLLOUT, 0},
      {output_pipe[0], POLLIN | POLLHUP, 0}, {diagnostic_pipe[0], POLLIN | POLLHUP, 0},
      {report_pipe[0], POLLIN | POLLHUP, 0}}};
    const int ready = ::poll(fds.data(), fds.size(), wait_ms);
    if (ready < 0 && errno != EINTR) stop(Status::failed, "could not poll worker descriptors");
    if (input_pair[1] >= 0 && (fds[0].revents & (POLLOUT | POLLERR | POLLHUP | POLLNVAL))) {
      const auto count = input.size() - written;
      const auto size = ::send(input_pair[1], input.data() + written,
                               count < 65536 ? count : 65536, MSG_NOSIGNAL);
      if (size > 0) {
        written += static_cast<std::size_t>(size);
        if (written == input.size()) close_fd(input_pair[1]);
      } else if (size == 0 || (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)) {
        stop(Status::failed, "worker closed input before the request was sent");
      }
    }
    if (output_pipe[0] >= 0) drain(output_pipe[0], result.output, output_limit, false);
    if (diagnostic_pipe[0] >= 0) drain(diagnostic_pipe[0], result.diagnostics, diagnostic_limit, true);
    if (report_pipe[0] >= 0) {
      char bytes[sizeof(Report)];
      const auto count = ::read(report_pipe[0], bytes, sizeof(bytes));
      if (count > 0) {
        if (report_size + static_cast<std::size_t>(count) > sizeof(report)) {
          stop(Status::failed, "invalid worker supervisor report");
          close_fd(report_pipe[0]);
        } else {
          for (ssize_t i = 0; i < count; ++i)
            reinterpret_cast<char*>(&report)[report_size++] = bytes[i];
        }
      } else if (count == 0) close_fd(report_pipe[0]);
      else if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
        stop(Status::failed, "could not read worker supervisor report");
        close_fd(report_pipe[0]);
      }
    }
  }
  close_fd(control_pipe[1]);
  close_fd(input_pair[1]);
  int supervisor_status = 0;
  pid_t waited;
  do { waited = ::waitpid(supervisor, &supervisor_status, 0); } while (waited < 0 && errno == EINTR);
  const bool cleanup_confirmed = waited == supervisor && report_size == sizeof(report) &&
      report.magic == report_magic && report.cleanup_error == 0 &&
      WIFEXITED(supervisor_status) && WEXITSTATUS(supervisor_status) == 0;
  if (!cleanup_confirmed) {
    result.status = Status::failed;
    result.error = "worker supervisor could not confirm complete process cleanup";
  } else if (!stopping) {
    if (report.startup_error != 0) {
      result.status = Status::failed;
      result.error = "could not start or execute worker";
    } else if (written != input.size()) {
      result.status = Status::failed;
      result.error = "worker exited before the request was sent";
    } else {
      result.status = Status::completed;
    }
  }
  if (report_size == sizeof(report) && report.magic == report_magic) {
    if (report.wait_status != -1) {
      if (WIFEXITED(report.wait_status)) result.exit_code = WEXITSTATUS(report.wait_status);
      else if (WIFSIGNALED(report.wait_status)) result.exit_code = 128 + WTERMSIG(report.wait_status);
    }
    if (report.deadline_triggered && cleanup_confirmed && report.startup_error == 0 && !failure_before_deadline) {
      result.status = Status::timed_out;
      result.error = "worker deadline exceeded";
    }
  }
  if (truncated_diagnostics) {
    constexpr char marker[] = "\n[diagnostics truncated]\n";
    const std::size_t length = sizeof(marker) - 1;
    result.diagnostics.replace(result.diagnostics.size() - length, length, marker, length);
  }
  close_all();
#endif
  result.elapsed = elapsed();
  return result;
}
}  // namespace ph::price::process
