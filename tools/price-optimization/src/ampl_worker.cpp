// SPDX-License-Identifier: LicenseRef-Praesidium-Humanitatis-Worker-Protection-1.0
#include "ph/price/integration.hpp"
#include <cerrno>
#include <fcntl.h>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unistd.h>

int main(int argc, char** argv) {
  using namespace ph::price;
  using namespace ph::price::integration;
  // Preserve the protocol pipe, then route all interpreter/solver stdout to
  // captured diagnostics. This includes output written by vendor C code.
  const int protocol = fcntl(STDOUT_FILENO, F_DUPFD_CLOEXEC, 3);
  if (protocol < 0 || dup2(STDERR_FILENO, STDOUT_FILENO) < 0) return 2;
  Json reply;
  std::string request_id;
  try {
    AmplConfig config;
    for (int i = 1; i < argc; ++i) {
      const std::string option = argv[i];
      if (i + 1 == argc) throw std::invalid_argument("missing worker option value");
      if (option == "--ampl-directory") config.binary_directory = argv[++i];
      else if (option == "--solver") config.solver = argv[++i];
      else throw std::invalid_argument("unknown worker option");
    }
    std::string input;
    char buffer[8192];
    while (std::cin) {
      std::cin.read(buffer, sizeof buffer);
      input.append(buffer, static_cast<std::size_t>(std::cin.gcount()));
      if (input.size() > kMaxJsonBytes) throw std::invalid_argument("request exceeds 8 MiB");
    }
    const auto parsed = parse_request(parse_json(input), unix_now());
    request_id = parsed.engine.request_id;
    auto backend = make_ampl_backend(config);
    reply = result_json(parsed, optimize(parsed.engine, *backend));
  } catch (const std::exception& error) {
    reply = failure_json(request_id, "invalid_input", "invalid_request", error.what());
  }
  const auto output = reply.dump() + '\n';
  std::size_t sent = 0;
  while (sent < output.size()) {
    const auto count = write(protocol, output.data() + sent, output.size() - sent);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) { close(protocol); return 2; }
    sent += static_cast<std::size_t>(count);
  }
  close(protocol);
  return 0;
}
