// SPDX-License-Identifier: MIT
#include "files.hpp"
#include <nlohmann/json.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using Json = nlohmann::json;
namespace fs = std::filesystem;
int checks = 0;
void require(bool condition, const std::string& message) {
  ++checks; if (!condition) throw std::runtime_error(message);
}
struct Call { int exit; Json output; };
Call call(std::vector<std::string> arguments) {
  arguments.insert(arguments.begin(), "exchange_simulation");
  std::vector<char*> argv; for (auto& argument : arguments) argv.push_back(argument.data());
  std::ostringstream output;
  auto* previous = std::cout.rdbuf(output.rdbuf());
  const int status = ph::exchange::files::cli(static_cast<int>(argv.size()), argv.data());
  std::cout.rdbuf(previous);
  return {status, Json::parse(output.str())};
}
std::string bytes(const fs::path& path) {
  std::ifstream input(path, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
}
Json read(const fs::path& path) { return Json::parse(bytes(path)); }
void write(const fs::path& path, const std::string& content) {
  fs::create_directories(path.parent_path());
  std::ofstream output(path, std::ios::binary);
  output << content; if (!output) throw std::runtime_error("test write failed");
}
void write(const fs::path& path, const Json& value) { write(path, value.dump(2)); }
}
int main() {
  fs::path directory;
  try {
    const auto token = std::chrono::steady_clock::now().time_since_epoch().count();
    directory = fs::temp_directory_path() / ("exchange-files-test-" + std::to_string(token));
    if (!fs::create_directory(directory)) throw std::runtime_error("cannot create unique test directory");
    const auto example = call({"--example-config"});
    require(example.exit == 0 && example.output.at("schema_version") == "exchange.run.v1", "example config is versioned");
    require(example.output == read(PH_EXCHANGE_SOURCE_CONFIG),
        "example config preserves the entire authoritative central cfg, including run identity and record paths");
    auto config = example.output;
    config["simulation"]["periods"] = 2;
    config["run_id"] = "test-run";
    config["records"] = {{"history_file", "../records/history.json"}, {"output_directory", "../runs/first"}};
    const auto cfg = directory / "configs" / "exchange.cfg";
    const auto history_file = directory / "records" / "history.json";
    const Json history = {{"schema_version", "exchange.history.v1"}, {"observations", Json::array()}};
    write(cfg, config); write(history_file, history);
    const auto first = call({"--config", cfg.string()});
    require(first.exit == 0 && first.output.at("status") == "ok", "valid file run succeeds: " + first.output.dump());
    const auto run = directory / "runs" / "first";
    const auto manifest = read(run / "manifest.json");
    require(manifest.at("status") == "complete" && manifest.at("simulation_schema") == "exchange.sim.v3", "manifest marks complete v3 run");
    require(manifest.at("run_id") == "test-run" && manifest.at("requested_periods") == 2, "manifest retains run identity and requested horizon");
    require(read(run / "configuration.cfg") == config && read(run / "history.json") == history, "input records snapshot exactly");
    const auto request = read(run / "request.json");
    require(request.at("config") == config.at("simulation") && request.at("history") == history, "request uses explicit config and history with no merge");
    const auto result = read(run / "result.json");
    std::size_t events = 0, daily_records = 0;
    for (const auto* mode : {"optimized", "fixed"}) {
      require(manifest.at("policies").at(mode) == result.at(mode).at("summary"), "terminal summary retained verbatim");
      events += result.at(mode).at("events").size();
      for (const auto& row : result.at(mode).at("rows")) {
        std::ostringstream name; name << "day-" << std::setfill('0') << std::setw(4) << row.at("day").get<int>() << ".json";
        require(read(run / "daily" / mode / name.str()) == row, "per-policy daily records preserve complete row");
        ++daily_records;
      }
    }
    const auto assurance = read(run / "assurance-events.json");
    require(assurance.at("events").size() == events, "assurance records contain every policy event");
    for (const auto& event : assurance.at("events"))
      require(event.contains("policy") && event.contains("event"), "assurance event preserves policy correlation");
    require(manifest.at("files").size() == 5 + daily_records, "manifest lists all output records except itself");
    for (const auto& file : manifest.at("files")) {
      const auto path = run / file.at("path").get<std::string>();
      require(fs::is_regular_file(path) && fs::file_size(path) == file.at("bytes").get<std::uintmax_t>(), "manifest record length verified");
    }
    const auto before = bytes(run / "result.json");
    const auto repeated = call({"--config", cfg.string()});
    require(repeated.exit == 2 && repeated.output.at("status") == "error", "existing output refuses overwrite");
    require(bytes(run / "result.json") == before, "failed overwrite preserves prior result");
    const auto assert_rejected = [&](const Json& candidate, const Json& data, const std::string& message) {
      auto next = candidate;
      next["records"]["output_directory"] = "../runs/rejected-" + std::to_string(checks);
      write(cfg, next); write(history_file, data);
      const auto response = call({"--config", cfg.string()});
      require(response.exit == 2 && response.output.at("status") == "error", message);
      require(!fs::exists(cfg.parent_path() / next["records"]["output_directory"].get<std::string>()), "invalid run creates no output");
    };
    auto invalid_config = config; invalid_config["unknown"] = 1;
    assert_rejected(invalid_config, history, "unknown run setting rejected");
    invalid_config = config; invalid_config["simulation"]["worker_wages"] = true;
    assert_rejected(invalid_config, history, "simulator rejects invalid nested config");
    invalid_config = config; invalid_config["records"]["history_file"] = "https://example.invalid/history.json";
    assert_rejected(invalid_config, history, "network paths rejected");
    const auto sku = config.at("simulation").at("products")[0].at("sku");
    const Json record = {{"day", -1}, {"sku", sku}, {"price", 200}, {"sales_units", 10}, {"stockout", false}};
    auto bad_history = history; bad_history["observations"] = Json::array({record});
    bad_history["observations"][0]["day"] = 0;
    assert_rejected(config, bad_history, "nonnegative pre-run day rejected");
    bad_history["observations"][0] = record; bad_history["observations"].push_back(record);
    assert_rejected(config, bad_history, "duplicate SKU/day rejected");
    bad_history["observations"] = Json::array({record}); bad_history["observations"][0]["sku"] = "unknown";
    assert_rejected(config, bad_history, "unknown historical SKU rejected");
    bad_history["observations"][0] = record; bad_history["observations"][0]["stockout"] = 1;
    assert_rejected(config, bad_history, "stockout requires boolean");
    bad_history["observations"][0] = record; bad_history["observations"][0]["price"] = 200.0;
    assert_rejected(config, bad_history, "history money requires integer");
    write(cfg, std::string("{\"schema_version\":\"exchange.run.v1\",\"schema_version\":\"exchange.run.v1\"}"));
    require(call({"--config", cfg.string()}).exit == 2, "duplicate file keys rejected");
    require(call({"--unknown"}).exit == 2, "unknown CLI option rejected");
    std::cout << checks << " file-run checks passed; test records preserved at " << directory << '\n';
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << "; test records preserved at " << directory << '\n';
    return 1;
  }
}
