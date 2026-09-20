// SPDX-License-Identifier: LicenseRef-Praesidium-Humanitatis-Worker-Protection-1.0
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {
void write_all(int fd, const char* data, std::size_t size) {
  while (size != 0) {
    const auto count = ::write(fd, data, size);
    if (count > 0) { data += count; size -= static_cast<std::size_t>(count); }
    else if (count < 0 && errno == EINTR) continue;
    else ::_exit(90);
  }
}
void pid_line() {
  const std::string line = "PID " + std::to_string(::getpid()) + "\n";
  write_all(STDOUT_FILENO, line.data(), line.size());
}
[[noreturn]] void forever() { for (;;) ::pause(); }
}

int main(int argc, char** argv) {
  if (argc < 2) return 91;
  std::string mode = argv[1];
  if (mode == "tree-file") {
    if (argc != 3) return 100;
    const int fd = ::open(argv[2], O_WRONLY | O_APPEND | O_CLOEXEC);
    if (fd < 0 || ::dup2(fd, STDOUT_FILENO) < 0) return 101;
    ::close(fd);
    mode = "tree";
  }
  if (mode == "echo") {
    std::array<char, 8192> bytes {};
    for (;;) {
      const auto size = ::read(STDIN_FILENO, bytes.data(), bytes.size());
      if (size > 0) write_all(STDOUT_FILENO, bytes.data(), static_cast<std::size_t>(size));
      else if (size < 0 && errno == EINTR) continue;
      else if (size == 0) break;
      else return 92;
    }
    constexpr char diagnostic[] = "fixture diagnostic\n";
    write_all(STDERR_FILENO, diagnostic, sizeof(diagnostic) - 1);
    return 0;
  }
  if (mode == "argument") {
    if (argc != 3) return 93;
    write_all(STDOUT_FILENO, argv[2], std::strlen(argv[2]));
    return 0;
  }
  if (mode == "exit") return 17;
  if (mode == "sleep" || mode == "backpressure") { pid_line(); forever(); }
  if (mode == "close-input") { ::close(STDIN_FILENO); pid_line(); forever(); }
  if (mode == "tree" || mode == "orphan" || mode == "escaped") {
    pid_line();
    int handshake[2];
    if (::pipe(handshake) != 0) return 94;
    const auto child = ::fork();
    if (child < 0) return 95;
    if (child == 0) {
      ::close(handshake[0]);
      if (mode == "escaped" && ::setsid() < 0) ::_exit(96);
      pid_line();
      const auto grandchild = ::fork();
      if (grandchild < 0) ::_exit(97);
      if (grandchild == 0) {
        pid_line();
        write_all(handshake[1], "x", 1);
        ::close(handshake[1]);
        forever();
      }
      ::close(handshake[1]);
      forever();
    }
    ::close(handshake[1]);
    char ready;
    if (::read(handshake[0], &ready, 1) != 1) return 98;
    ::close(handshake[0]);
    if (mode == "orphan" || mode == "escaped") return 0;
    forever();
  }
  if (mode == "flood" || mode == "stderr-flood") {
    std::array<char, 8192> bytes;
    bytes.fill('x');
    const int fd = mode == "flood" ? STDOUT_FILENO : STDERR_FILENO;
    for (int i = 0; i < (mode == "flood" ? 4096 : 32); ++i)
      write_all(fd, bytes.data(), bytes.size());
    if (mode == "stderr-flood") write_all(STDOUT_FILENO, "done", 4);
    return 0;
  }
  return 99;
}
