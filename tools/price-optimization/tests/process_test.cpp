// SPDX-License-Identifier: LicenseRef-Praesidium-Humanitatis-Worker-Protection-1.0
#include "ph/price/process.hpp"
#include <cerrno>
#include <chrono>
#include <dirent.h>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <signal.h>
#include <sys/types.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {
int assertions = 0;
void require(bool condition, const char* message) {
  ++assertions;
  if (!condition) throw std::runtime_error(message);
}
void processes_gone(const std::string& output, std::size_t expected) {
  std::istringstream stream(output);
  std::string label;
  long pid = 0;
  std::size_t count = 0;
  while (stream >> label >> pid) {
    require(label == "PID" && pid > 1, "fixture returned a malformed process ID");
    ++count;
    errno = 0;
    require(::kill(static_cast<pid_t>(pid), 0) == -1 && errno == ESRCH,
            "owned worker/descendant remains alive or is an unreaped zombie");
    const auto path = "/proc/" + std::to_string(pid);
    require(::access(path.c_str(), F_OK) == -1 && errno == ENOENT,
            "owned worker/descendant still has a /proc entry");
  }
  require(count == expected, "fixture did not report every expected process");
}
int descriptor_count() {
  DIR* directory = ::opendir("/proc/self/fd");
  if (!directory) throw std::runtime_error("cannot inspect test file descriptors");
  int count = 0;
  while (const auto* entry = ::readdir(directory))
    if (entry->d_name[0] != '.') ++count;
  ::closedir(directory);
  return count;
}

std::string read_file(const std::string& path) {
  std::ifstream file(path);
  return std::string(std::istreambuf_iterator<char>(file), {});
}

void parent_death(const std::string& fixture) {
  using namespace std::chrono_literals;
  int previous_subreaper = 0;
  require(::prctl(PR_GET_CHILD_SUBREAPER, &previous_subreaper, 0, 0, 0) == 0 &&
      ::prctl(PR_SET_CHILD_SUBREAPER, 1, 0, 0, 0) == 0, "cannot adopt the parent-death test supervisor");
  char filename[] = "/tmp/ph-price-process-XXXXXX";
  const int fd = ::mkstemp(filename);
  require(fd >= 0, "cannot create parent-death fixture record");
  ::close(fd);
  const pid_t host = ::fork();
  require(host >= 0, "cannot create parent-death fixture host");
  if (host == 0) {
    ph::price::process::run({fixture, "tree-file", filename}, {}, 10s);
    ::_exit(0);
  }
  std::string output;
  for (int i = 0; i < 200; ++i) {
    output = read_file(filename);
    std::size_t lines = 0;
    for (char ch : output) if (ch == '\n') ++lines;
    if (lines == 3) break;
    ::usleep(5000);
  }
  const auto child_file = "/proc/" + std::to_string(host) + "/task/" + std::to_string(host) + "/children";
  std::istringstream children(read_file(child_file));
  pid_t supervisor = -1;
  children >> supervisor;
  const bool killed = ::kill(host, SIGKILL) == 0;
  int status = 0;
  const bool host_reaped = ::waitpid(host, &status, 0) == host;
  bool supervisor_reaped = false;
  if (supervisor > 1) {
    for (int i = 0; i < 400; ++i) {
      const pid_t reaped = ::waitpid(supervisor, &status, WNOHANG);
      if (reaped == supervisor) { supervisor_reaped = true; break; }
      ::usleep(5000);
    }
  }
  ::unlink(filename);
  ::prctl(PR_SET_CHILD_SUBREAPER, previous_subreaper, 0, 0, 0);
  require(killed && host_reaped && supervisor > 1 && supervisor_reaped,
          "parent death left its isolated supervisor running or unreaped");
  processes_gone(output, 3);
}

void independent_deadline(const std::string& fixture) {
  using namespace std::chrono_literals;
  char filename[] = "/tmp/ph-price-deadline-XXXXXX";
  const int record = ::mkstemp(filename);
  require(record >= 0, "cannot create independent deadline fixture record");
  ::close(record);
  int observation[2];
  require(::pipe(observation) == 0, "cannot create independent deadline observer pipe");
  const auto started = std::chrono::steady_clock::now();
  const pid_t observer = ::fork();
  require(observer >= 0, "cannot create independent deadline observer");
  if (observer == 0) {
    ::close(observation[0]);
    long elapsed_ms = -1;
    for (int attempt = 0; attempt < 400; ++attempt) {
      std::istringstream records(read_file(filename));
      std::string label;
      long pid = 0;
      int count = 0;
      bool gone = true;
      while (records >> label >> pid) {
        ++count;
        errno = 0;
        if (label != "PID" || pid <= 1 || ::kill(static_cast<pid_t>(pid), 0) != -1 || errno != ESRCH)
          gone = false;
      }
      if (count == 3 && gone) {
        elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started).count();
        break;
      }
      ::usleep(1000);
    }
    const auto ignored = ::write(observation[1], &elapsed_ms, sizeof(elapsed_ms));
    (void)ignored;
    ::_exit(0);
  }
  ::close(observation[1]);
  int callbacks = 0;
  const auto outcome = ph::price::process::run({fixture, "tree-file", filename}, {}, 30ms, [&] {
    if (++callbacks == 2) ::usleep(150000);
    return false;
  });
  long observed_ms = -1;
  const auto received = ::read(observation[0], &observed_ms, sizeof(observed_ms));
  ::close(observation[0]);
  int status = 0;
  const bool reaped = ::waitpid(observer, &status, 0) == observer;
  const auto output = read_file(filename);
  ::unlink(filename);
  require(received == sizeof(observed_ms) && reaped && WIFEXITED(status), "independent observer failed");
  require(outcome.status == ph::price::process::Status::timed_out && outcome.elapsed >= 150ms,
          "slow callback did not retain the timeout classification");
  require(observed_ms >= 0 && observed_ms < 120,
          "worker survived its deadline while the parent callback was sleeping");
  processes_gone(output, 3);
}
}

int main(int argc, char** argv) {
  using namespace std::chrono_literals;
  using ph::price::process::run;
  using ph::price::process::Status;
  try {
    require(argc == 2, "fixture absolute path argument is required");
    const std::string fixture = argv[1];
    const int descriptors_before = descriptor_count();
    const auto normal = run({fixture, "echo"}, "hello\n", 2s);
    require(normal.status == Status::completed && normal.exit_code == 0, "normal worker failed");
    require(normal.output == "hello\n" && normal.diagnostics == "fixture diagnostic\n", "capture mismatch");
    const auto empty = run({fixture, "echo"}, {}, 2s);
    require(empty.status == Status::completed && empty.output.empty(), "zero-length input was not closed");
    const std::string large_input(1024 * 1024, 'a');
    const auto large = run({fixture, "echo"}, large_input, 3s);
    require(large.status == Status::completed && large.output == large_input, "short IO/backpressure lost data");
    const std::string literal = "$(this-is-not-a-shell); 'quoted' *";
    const auto argument = run({fixture, "argument", literal}, {}, 2s);
    require(argument.status == Status::completed && argument.output == literal, "arguments were interpreted by a shell");
    const auto nonzero = run({fixture, "exit"}, {}, 2s);
    require(nonzero.status == Status::completed && nonzero.exit_code == 17, "exit code was not preserved");
    const auto missing = run({"/no/such/ph-price-worker"}, {}, 2s);
    require(missing.status == Status::failed && !missing.error.empty(), "exec failure was not distinguished");
    const auto relative = run({"relative-worker"}, {}, 2s);
    require(relative.status == Status::failed, "relative executable was accepted");
    const auto nul = run({fixture, std::string("bad\0argument", 12)}, {}, 2s);
    require(nul.status == Status::failed, "NUL argument was accepted");
    const auto invalid_timeout = run({fixture, "echo"}, {}, 0ms);
    require(invalid_timeout.status == Status::failed, "nonpositive deadline was accepted");
    const auto too_large = run({fixture, "echo"}, std::string(8 * 1024 * 1024 + 1, 'x'), 2s);
    require(too_large.status == Status::failed, "oversized request was accepted");

    const auto timeout = run({fixture, "tree"}, {}, 150ms);
    require(timeout.status == Status::timed_out && timeout.elapsed < 2s, "tree deadline was not bounded");
    processes_gone(timeout.output, 3);
    const auto cancel_at = std::chrono::steady_clock::now() + 150ms;
    const auto cancelled = run({fixture, "tree"}, {}, 3s,
        [&] { return std::chrono::steady_clock::now() >= cancel_at; });
    require(cancelled.status == Status::cancelled && cancelled.elapsed < 2s, "tree cancellation failed");
    processes_gone(cancelled.output, 3);
    const auto immediate = run({fixture, "tree"}, {}, 2s, [] { return true; });
    require(immediate.status == Status::cancelled && immediate.output.empty(), "pre-cancelled request started a worker");
    const auto throwing = run({fixture, "tree"}, {}, 2s, []() -> bool { throw std::runtime_error("cancel error"); });
    require(throwing.status == Status::failed, "throwing callback escaped the process runner");
    const auto throw_at = std::chrono::steady_clock::now() + 150ms;
    const auto throwing_after_spawn = run({fixture, "tree"}, {}, 2s, [&]() -> bool {
      if (std::chrono::steady_clock::now() >= throw_at) throw std::runtime_error("late cancel error");
      return false;
    });
    require(throwing_after_spawn.status == Status::failed, "callback failure after spawn was not contained");
    processes_gone(throwing_after_spawn.output, 3);
    for (const std::string mode : {"orphan", "escaped"}) {
      const auto orphan = run({fixture, mode}, {}, 2s);
      require(orphan.status == Status::completed && orphan.exit_code == 0, "leader completion failed");
      processes_gone(orphan.output, 3);
    }
    const auto backpressure = run({fixture, "backpressure"}, std::string(8 * 1024 * 1024, 'x'), 150ms);
    require(backpressure.status == Status::timed_out && backpressure.elapsed < 2s, "input backpressure bypassed deadline");
    processes_gone(backpressure.output, 1);
    const auto closed_input = run({fixture, "close-input"}, std::string(1024 * 1024, 'x'), 2s);
    require(closed_input.status == Status::failed, "early closed stdin was accepted");
    processes_gone(closed_input.output, 1);
    const auto flood = run({fixture, "flood"}, {}, 3s);
    require(flood.status == Status::failed && flood.output.size() == 8 * 1024 * 1024, "stdout cap was not enforced");
    const auto diagnostics = run({fixture, "stderr-flood"}, {}, 2s);
    require(diagnostics.status == Status::completed && diagnostics.output == "done", "stderr was not drained");
    require(diagnostics.diagnostics.size() == 64 * 1024 &&
        diagnostics.diagnostics.find("[diagnostics truncated]") != std::string::npos, "stderr cap/truncation not reported");
    parent_death(fixture);
    independent_deadline(fixture);
    require(descriptor_count() == descriptors_before, "process runner leaked descriptors");
    std::cout << assertions << " process assertions passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "process test failure: " << error.what() << '\n';
    return 1;
  }
}
