// SPDX-License-Identifier: LicenseRef-Praesidium-Humanitatis-Worker-Protection-1.0
// Explicit test fixture only. Never selected automatically by the backend.
#include "ph/price/integration.hpp"
#include <iostream>
#include <iterator>
#include <unistd.h>

int main(int argc, char** argv) {
  using namespace ph::price;
  using namespace ph::price::integration;
  std::string mode;
  for (int i = 1; i + 1 < argc; i += 2) if (std::string(argv[i]) == "--ampl-directory") mode = argv[i + 1];
  const std::string input((std::istreambuf_iterator<char>(std::cin)), std::istreambuf_iterator<char>());
  if (mode == "/fixture/hang") { for (;;) pause(); }
  if (mode == "/fixture/malformed") { std::cout << "not-json\n"; return 0; }
  const auto parsed = parse_request(parse_json(input), unix_now());
  if (mode == "/fixture/invalid-diagnostic") std::cerr << "diagnostic " << char(0xff) << '\n';
  if (mode == "/fixture/failure") {
    std::cout << result_json(parsed, {SolveStatus::solver_failed, "fixture failure", std::nullopt}).dump() << '\n';
    return 0;
  }
  const auto evaluated = evaluate_selection(parsed.engine, {1, 1}, unix_now());
  auto result = result_json(parsed, {SolveStatus::recommended, "explicit fixture selection", evaluated.recommendation});
  if (mode == "/fixture/forged") result["recommendation"]["products"][0]["candidate_index"] = 3;
  if (mode == "/fixture/wrong-id") result["request_id"] = "different-request";
  if (mode == "/fixture/wrong-snapshot") result["input_snapshot"]["currency"] = "USD";
  if (mode == "/fixture/wrong-model") result["model_sha256"] = std::string(64, '0');
  if (mode == "/fixture/wrong-objective") result["objective"]["id"] = "different_objective";
  std::cout << result.dump() << '\n';
  return 0;
}
