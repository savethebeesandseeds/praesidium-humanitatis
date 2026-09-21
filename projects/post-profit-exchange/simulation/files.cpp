// SPDX-License-Identifier: MIT
#include "files.hpp"
#include "simulation.hpp"
#include "default_config.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace ph::exchange::files {
namespace {
using Json = nlohmann::json;
namespace fs = std::filesystem;
constexpr std::size_t max_file_bytes = 8 * 1024 * 1024;
[[noreturn]] void invalid(const std::string& message) { throw std::invalid_argument(message); }
void fields(const Json& object, std::initializer_list<const char*> keys, const char* path) {
  if (!object.is_object() || object.size() != keys.size()) invalid(std::string(path) + " has unknown or missing fields");
  for (const auto* key : keys) if (!object.contains(key)) invalid(std::string(path) + " missing " + key);
}
Json parse(const std::string& text) {
  if (text.size() > max_file_bytes) invalid("JSON file exceeds 8 MiB");
  std::vector<std::set<std::string>> keys;
  auto callback = [&](int, Json::parse_event_t event, Json& value) {
    if (event == Json::parse_event_t::object_start || event == Json::parse_event_t::array_start) {
      keys.emplace_back(); if (keys.size() > 32) invalid("JSON file nesting exceeds 32");
    } else if (event == Json::parse_event_t::key) {
      if (!keys.back().insert(value.get<std::string>()).second) invalid("duplicate JSON key");
    } else if (event == Json::parse_event_t::object_end || event == Json::parse_event_t::array_end) keys.pop_back();
    return true;
  };
  return Json::parse(text, callback);
}
Json read_json(const fs::path& path) {
  if (!fs::is_regular_file(path)) invalid("expected a regular JSON file: " + path.string());
  if (fs::file_size(path) > max_file_bytes) invalid("JSON file exceeds 8 MiB: " + path.string());
  std::ifstream stream(path, std::ios::binary);
  if (!stream) invalid("cannot open JSON file: " + path.string());
  std::string text;
  char buffer[8192];
  while (stream) {
    stream.read(buffer, sizeof buffer);
    text.append(buffer, static_cast<std::size_t>(stream.gcount()));
    if (text.size() > max_file_bytes) invalid("JSON file grew beyond 8 MiB");
  }
  if (!stream.eof()) invalid("cannot finish reading JSON file: " + path.string());
  return parse(text);
}
std::string text(const Json& json, const char* label, std::size_t maximum = 1024) {
  if (!json.is_string()) invalid(std::string(label) + " must be a string");
  const auto value = json.get<std::string>();
  if (value.empty() || value.size() > maximum || !std::all_of(value.begin(), value.end(), [](unsigned char c) { return c >= 32 && c != 127; }))
    invalid(std::string(label) + " is empty, oversized or contains controls");
  return value;
}
std::int64_t integer(const Json& value, std::int64_t minimum, std::int64_t maximum, const char* label) {
  if (!value.is_number_integer() || (value.is_number_unsigned() && value.get<std::uint64_t>() >
      static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))) invalid(std::string(label) + " must be an integer");
  const auto number = value.get<std::int64_t>();
  if (number < minimum || number > maximum) invalid(std::string(label) + " is out of range");
  return number;
}
fs::path local_path(const std::string& value, const fs::path& base) {
  if (value.find("://") != std::string::npos || value.rfind("//", 0) == 0 || value.rfind("\\\\", 0) == 0)
    invalid("record paths must be local filesystem paths, not URLs or network shares");
  fs::path path = fs::u8path(value);
  if (path.is_relative()) path = base / path;
  return fs::absolute(path).lexically_normal();
}
void validate_history(const Json& history, const Json& simulation) {
  fields(history, {"schema_version", "observations"}, "history");
  if (history.at("schema_version") != "exchange.history.v1") invalid("unsupported history schema_version");
  if (!history.at("observations").is_array() || history.at("observations").size() > 12000)
    invalid("history observations must be an array of at most 12000 records");
  std::set<std::string> skus;
  if (!simulation.is_object() || !simulation.contains("products") || !simulation.at("products").is_array())
    invalid("simulation products are missing");
  for (const auto& product : simulation.at("products")) skus.insert(text(product.at("sku"), "product sku", 64));
  std::map<std::string, std::int64_t> latest;
  for (const auto& observation : history.at("observations")) {
    fields(observation, {"day", "sku", "price", "sales_units", "stockout"}, "history observation");
    const auto sku = text(observation.at("sku"), "history sku", 64);
    if (!skus.count(sku)) invalid("history references an unknown SKU: " + sku);
    const auto day = integer(observation.at("day"), -1000000, -1, "history day");
    if (latest.count(sku) && day <= latest.at(sku))
      invalid("history days must strictly increase per SKU without duplicate day/SKU records");
    latest[sku] = day;
    integer(observation.at("price"), 1, 1000000, "history price");
    integer(observation.at("sales_units"), 0, 1000000, "history sales_units");
    if (!observation.at("stockout").is_boolean()) invalid("history stockout must be boolean");
  }
}
Json example_config() {
  return parse(kExampleRunConfiguration);
}
void exclusive_write(const fs::path& path, const Json& value, Json& outputs, const fs::path& output_root) {
  const auto bytes = value.dump(2) + '\n';
  fs::create_directories(path.parent_path());
#ifdef _WIN32
  FILE* stream = _wfopen(path.c_str(), L"wbx");
#else
  FILE* stream = std::fopen(path.c_str(), "wbx");
#endif
  if (!stream) throw std::runtime_error("cannot exclusively create " + path.string() + ": " + std::strerror(errno));
  const bool written = std::fwrite(bytes.data(), 1, bytes.size(), stream) == bytes.size();
  const bool closed = std::fclose(stream) == 0;
  if (!written || !closed) throw std::runtime_error("cannot finish writing " + path.string());
  outputs.push_back({{"path", fs::relative(path, output_root).generic_string()}, {"bytes", bytes.size()}});
}
Json execute_config(const fs::path& config_path) {
  const auto cfg_path = fs::absolute(config_path).lexically_normal();
  const auto config = read_json(cfg_path);
  fields(config, {"schema_version", "run_id", "simulation", "records"}, "run configuration");
  if (config.at("schema_version") != "exchange.run.v1") invalid("unsupported run schema_version");
  const auto id = text(config.at("run_id"), "run_id", 64);
  if (!std::all_of(id.begin(), id.end(), [](unsigned char c) { return (c >= 'a' && c <= 'z') ||
      (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.'; }))
    invalid("run_id may contain only letters, digits, dash, underscore and dot");
  fields(config.at("records"), {"history_file", "output_directory"}, "records");
  const auto history_path = local_path(text(config.at("records").at("history_file"), "history_file"), cfg_path.parent_path());
  const auto output_path = local_path(text(config.at("records").at("output_directory"), "output_directory"), cfg_path.parent_path());
  if (fs::exists(fs::symlink_status(output_path))) invalid("output directory already exists; records preserved: " + output_path.string());
  const auto history = read_json(history_path);
  validate_history(history, config.at("simulation"));
  const Json request = {{"op", "simulate"}, {"config", config.at("simulation")}, {"history", history}};
  // The bounded simulator produces this response locally; its complete result
  // can be larger than the external configuration/history file-size limit.
  const auto result = Json::parse(run_json(request.dump()));
  if (!result.is_object() || result.value("status", "") != "ok")
    invalid("simulation rejected the run: " + result.value("error", Json::object()).value("message", "unknown error"));
  if (result.value("schema_version", "") != "exchange.sim.v3") invalid("file runs require simulator schema exchange.sim.v3");
  // Invalid inputs produce no output. Once a fresh directory is claimed,
  // failed writes preserve it for inspection instead of deleting records.
  fs::create_directories(output_path.parent_path());
  if (!fs::create_directory(output_path)) invalid("output directory appeared concurrently; records preserved");
  Json outputs = Json::array();
  try {
    exclusive_write(output_path / "configuration.cfg", config, outputs, output_path);
    exclusive_write(output_path / "history.json", history, outputs, output_path);
    exclusive_write(output_path / "request.json", request, outputs, output_path);
    exclusive_write(output_path / "result.json", result, outputs, output_path);
    Json events = Json::array(), policy_summaries = Json::object();
    for (const auto* policy : {"optimized", "fixed"}) {
      const auto& path = result.at(policy);
      if (!path.at("rows").is_array() || !path.at("events").is_array()) invalid("simulator omitted rows or assurance events");
      for (const auto& row : path.at("rows")) {
        std::ostringstream filename;
        filename << "day-" << std::setfill('0') << std::setw(4) << integer(row.at("day"), 1, 365, "result day") << ".json";
        exclusive_write(output_path / "daily" / policy / filename.str(), row, outputs, output_path);
      }
      for (const auto& event : path.at("events")) events.push_back({{"policy", policy}, {"event", event}});
      policy_summaries[policy] = path.at("summary");
    }
    exclusive_write(output_path / "assurance-events.json", {{"schema_version", "exchange.assurance-log.v1"},
        {"run_id", id}, {"events", events}}, outputs, output_path);
    // The manifest is written last. Its absence marks a preserved partial run.
    const Json manifest = {{"schema_version", "exchange.manifest.v1"}, {"run_id", id}, {"status", "complete"},
        {"simulation_schema", result.at("schema_version")}, {"source_configuration", cfg_path.generic_string()},
        {"source_history", history_path.generic_string()}, {"requested_periods", config.at("simulation").at("periods")},
        {"policies", policy_summaries}, {"files", outputs}};
    Json unused = Json::array();
    exclusive_write(output_path / "manifest.json", manifest, unused, output_path);
    return {{"status", "ok"}, {"run_id", id}, {"output_directory", output_path.generic_string()},
        {"manifest", (output_path / "manifest.json").generic_string()}, {"policies", policy_summaries}};
  } catch (const std::exception& error) {
    throw std::runtime_error("incomplete run preserved at " + output_path.string() + ": " + error.what());
  }
}
}  // namespace
int cli(int argc, char** argv) {
  try {
    if (argc == 1) {
      const std::string command((std::istreambuf_iterator<char>(std::cin)), std::istreambuf_iterator<char>());
      std::cout << run_json(command) << '\n';
      return 0;
    }
    if (argc == 2 && std::string(argv[1]) == "--example-config") {
      std::cout << example_config().dump(2) << '\n'; return 0;
    }
    if (argc == 2 && std::string(argv[1]) == "--help") {
      std::cout << "exchange_simulation [--config PATH | --example-config]\n"
          "Without options: read one simulator JSON command from stdin.\n"
          "--config: read a JSON .cfg and referenced history, then write a fresh run directory.\n"
          "--example-config: emit the complete current defaults in the file-run contract.\n";
      return 0;
    }
    if (argc == 3 && std::string(argv[1]) == "--config") {
      std::cout << execute_config(local_path(argv[2], fs::current_path())).dump() << '\n'; return 0;
    }
    invalid("usage: exchange_simulation [--config PATH | --example-config]");
  } catch (const std::exception& error) {
    std::cout << Json{{"status", "error"}, {"error", {{"code", "file_run_failed"}, {"message", error.what()}}}}
        .dump(-1, ' ', false, Json::error_handler_t::replace) << '\n';
    return 2;
  }
}
}  // namespace ph::exchange::files
