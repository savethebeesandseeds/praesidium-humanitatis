// SPDX-License-Identifier: LicenseRef-Praesidium-Humanitatis-Worker-Protection-1.0
#pragma once
#include "ph/price/engine.hpp"
#include <nlohmann/json.hpp>
#include <string>

namespace ph::price::integration {
using Json = nlohmann::json;
inline constexpr const char* kSchemaVersion = "ph.price.v3";
inline constexpr const char* kModelVersion = "public-prices.v4";
inline constexpr const char* kObjectiveId = "operating_balance_tracking";
inline constexpr std::size_t kMaxJsonBytes = 8 * 1024 * 1024;

struct ParsedRequest {
  Request engine;
  Json snapshot;
  std::string policy_id;
  std::int64_t policy_version = 0;
};

// Reject duplicate keys, excessive nesting, non-JSON numbers and oversized input.
Json parse_json(const std::string& text);
// Strict versioned contract. Throws invalid_argument for schema/input errors.
ParsedRequest parse_request(const Json& json, Timestamp now);
// Serializes an independently checked result with exact financial explanations.
Json result_json(const ParsedRequest& request, const SolveResult& result);
// A stable envelope for failures without a parsed request.
Json failure_json(const std::string& request_id, const std::string& status,
                  const std::string& code, const std::string& message);
// Synthetic evaluation data with fresh timestamps, not an approved store policy.
Json synthetic_request_json(Timestamp now);
}  // namespace ph::price::integration
