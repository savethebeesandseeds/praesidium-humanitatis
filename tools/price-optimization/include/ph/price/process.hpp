// SPDX-License-Identifier: LicenseRef-Praesidium-Humanitatis-Worker-Protection-1.0
#pragma once
#include <chrono>
#include <functional>
#include <string>
#include <vector>

namespace ph::price::process {
enum class Status { completed, timed_out, cancelled, failed };
struct Result {
  Status status = Status::failed;
  int exit_code = -1;
  std::string output;
  std::string diagnostics;
  std::string error;
  std::chrono::milliseconds elapsed{0};
};
// Linux: execute argv directly (no shell), send input then close stdin, capture
// bounded stdout/stderr, enforce a monotonic deadline, and kill/reap the process
// group on cancellation, timeout, error or completion. The callback must not block.
Result run(const std::vector<std::string>& argv, const std::string& input,
           std::chrono::milliseconds timeout,
           const std::function<bool()>& cancelled = {});
}  // namespace ph::price::process
