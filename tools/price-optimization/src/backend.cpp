// SPDX-License-Identifier: LicenseRef-Praesidium-Humanitatis-Worker-Protection-1.0
#include "ph/price/integration.hpp"
#include "ph/price/process.hpp"
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <csignal>
#include <filesystem>
#include <fcntl.h>
#include <iostream>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <unistd.h>

namespace {
using namespace ph::price;
using namespace ph::price::integration;
volatile std::sig_atomic_t stopping = 0;
void stop(int) { stopping = 1; }

void fields(const Json& object, std::initializer_list<const char*> names) {
  if (!object.is_object() || object.size() != names.size())
    throw std::invalid_argument("command has missing or unknown fields");
  for (const auto* name : names) if (!object.contains(name))
    throw std::invalid_argument("command has missing or unknown fields");
}
std::string identity(const Json& value) {
  if (value.is_object() && value.contains("request_id") && value["request_id"].is_string()) {
    const auto id = value["request_id"].get<std::string>();
    if (id.size() <= 256) return id;
  }
  return {};
}
void nonblocking(int fd) {
  const int flags = fcntl(fd, F_GETFL);
  if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
    throw std::runtime_error("cannot configure protocol pipe");
}

class Service {
 public:
  Service(std::string worker, std::string runtime, std::string solver)
      : worker_(std::move(worker)), runtime_(std::move(runtime)), solver_(std::move(solver)) {}

  int serve() {
    nonblocking(STDIN_FILENO);
    nonblocking(STDOUT_FILENO);
    auto progress = std::chrono::steady_clock::now();
    while (!broken_ && !stopping) {
      const auto remaining = output_.size() - output_offset_;
      drain();
      if (remaining != output_.size() - output_offset_) progress = std::chrono::steady_clock::now();
      pump();
      if (eof_ && input_.empty() && output_.empty()) break;
      // A disconnected or non-reading client must not retain a service forever.
      if (!output_.empty() && std::chrono::steady_clock::now() - progress > std::chrono::seconds(5)) {
        broken_ = true;
        break;
      }
      if (output_.empty()) progress = std::chrono::steady_clock::now();
      pollfd fds[2]{{STDIN_FILENO, static_cast<short>(eof_ ? 0 : POLLIN), 0},
                    {STDOUT_FILENO, static_cast<short>(output_.empty() ? 0 : POLLOUT), 0}};
      if (poll(fds, 2, 20) < 0 && errno != EINTR) throw std::runtime_error("protocol poll failed");
    }
    drain();
    return broken_ ? 1 : 0;
  }

 private:
  void emit(const Json& event) {
    if (broken_) return;
    // Vendor diagnostics may contain arbitrary bytes. JSON remains valid UTF-8;
    // validated input strings are unchanged, invalid diagnostic bytes replaced.
    auto text = event.dump(-1, ' ', false, Json::error_handler_t::replace) + '\n';
    if (output_offset_) { output_.erase(0, output_offset_); output_offset_ = 0; }
    if (output_.size() + text.size() > 2 * kMaxJsonBytes) { broken_ = true; return; }
    output_ += text;
    drain();
  }
  void drain() {
    // Never block a cancellation/deadline callback on the dashboard reader.
    while (output_offset_ < output_.size()) {
      const auto n = write(STDOUT_FILENO, output_.data() + output_offset_, output_.size() - output_offset_);
      if (n < 0 && errno == EINTR) continue;
      if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
      if (n <= 0) { broken_ = true; return; }
      output_offset_ += static_cast<std::size_t>(n);
    }
    output_.clear(); output_offset_ = 0;
  }
  void reject(const std::string& id, const std::string& code, const std::string& message) {
    emit({{"event", "rejected"}, {"schema_version", kSchemaVersion}, {"request_id", id},
          {"error", {{"code", code}, {"message", message}}}});
  }
  void pump() {
    // Bound work per callback, including floods of tiny commands.
    char buffer[8192];
    for (int batch = 0; batch < 8 && !eof_ && !broken_; ++batch) {
      const auto n = read(STDIN_FILENO, buffer, sizeof buffer);
      if (n < 0 && errno == EINTR) continue;
      if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
      if (n < 0) { broken_ = true; return; }
      if (n == 0) { eof_ = true; break; }
      for (ssize_t i = 0; i < n; ++i) {
        const char c = buffer[i];
        input_ += c;
        if (input_.size() > kMaxJsonBytes) {
          input_.clear();
          reject({}, "input_buffer_limit", "protocol input buffer exceeds 8 MiB");
          broken_ = true;
          return;
        }
      }
    }
    for (int count = 0; count < 16 && !broken_ && !stopping; ++count) {
      const auto end = input_.find('\n');
      if (end == std::string::npos) break;
      auto line = input_.substr(0, end);
      input_.erase(0, end + 1);
      command(line);
    }
    if (eof_ && !input_.empty() && input_.find('\n') == std::string::npos) {
      input_.clear();
      reject({}, "incomplete_frame", "commands must end with a newline");
    }
  }
  void command(const std::string& line) {
    std::string id;
    if (!active_id_.empty() && line.size() > 4096) {
      reject({}, "active_command_too_large", "commands during a solve are limited to 4096 bytes");
      return;
    }
    try {
      const auto value = parse_json(line);
      if (!value.is_object() || !value.contains("op") || !value["op"].is_string())
        throw std::invalid_argument("command requires an op string");
      const std::string op = value["op"];
      if (op == "ping") {
        fields(value, {"op"});
        emit({{"event", "pong"}, {"schema_version", kSchemaVersion}, {"active_request_id", active_id_}});
      } else if (op == "cancel") {
        fields(value, {"op", "request_id"});
        id = identity(value);
        if (id.empty()) throw std::invalid_argument("cancel requires a nonempty request_id");
        if (active_id_ != id || active_id_.empty()) reject(id, "not_found", "no active request with that identifier");
        else {
          cancelled_ = true;
          emit({{"event", "cancellation_requested"}, {"schema_version", kSchemaVersion}, {"request_id", id}});
        }
      } else if (op == "solve") {
        fields(value, {"op", "timeout_ms", "request"});
        id = identity(value["request"]);
        if (!active_id_.empty()) { reject(id, "busy", "one solve is already active; cancel it or wait"); return; }
        if (!value["timeout_ms"].is_number_integer() || value["timeout_ms"].is_boolean() ||
            value["timeout_ms"] < 1 || value["timeout_ms"] > 60000)
          throw std::invalid_argument("timeout_ms must be an integer from 1 through 60000");
        ParsedRequest parsed;
        try { parsed = parse_request(value["request"], unix_now()); }
        catch (const std::exception& error) {
          emit(failure_json(id, "invalid_input", "invalid_request", error.what())); return;
        }
        try { solve(parsed, value["timeout_ms"].get<int>()); }
        catch (const std::exception& error) {
          emit(failure_json(parsed.engine.request_id, "solver_failed", "backend_failed", error.what()));
        }
      } else throw std::invalid_argument("unsupported command");
    } catch (const std::exception& error) {
      reject(id, "invalid_command", error.what());
    }
  }

  Json checked_reply(const ParsedRequest& parsed, const std::string& text) {
    const auto reply = parse_json(text);
    const auto expected_metadata = failure_json(parsed.engine.request_id, "solver_failed", "metadata", "metadata");
    if (reply.at("schema_version") != kSchemaVersion || reply.at("event") != "result" ||
        reply.at("request_id") != parsed.engine.request_id ||
        reply.at("model_version") != kModelVersion || reply.at("input_snapshot") != parsed.snapshot ||
        reply.at("model_sha256") != expected_metadata.at("model_sha256") ||
        reply.at("engine_version") != expected_metadata.at("engine_version") ||
        reply.at("objective") != expected_metadata.at("objective") ||
        reply.at("policy") != Json{{"id", parsed.policy_id}, {"version", parsed.policy_version}})
      throw std::runtime_error("worker reply identity or input snapshot does not match");
    const std::string status = reply.at("status").get<std::string>();
    if (status == "recommended") {
      const auto& recommendation = reply.at("recommendation");
      const auto& products = recommendation.at("products");
      if (!products.is_array() || products.size() != parsed.engine.products.size())
        throw std::runtime_error("worker returned incorrect product count");
      std::vector<std::size_t> indices;
      for (std::size_t i = 0; i < products.size(); ++i) {
        const auto& choice = products[i].at("candidate_index");
        if (products[i].at("sku") != parsed.engine.products[i].sku ||
            !choice.is_number_integer() || choice < 0 || choice >= parsed.engine.products[i].candidates.size())
          throw std::runtime_error("worker returned an invalid candidate identity");
        indices.push_back(choice.get<std::size_t>());
      }
      auto verified = evaluate_selection(parsed.engine, indices, unix_now());
      if (!verified.recommendation) throw std::runtime_error("worker selection failed independent validation or expired");
      const double reported = recommendation.at("expected").at("worker_surplus").get<double>();
      const auto expected = verified.recommendation->expected_worker_surplus;
      if (!std::isfinite(reported) || std::abs(reported - expected) > std::max(1e-5, std::abs(expected) * 1e-10))
        throw std::runtime_error("worker objective disagrees with independent recomputation");
      SolveResult result{SolveStatus::recommended, "independently validated recommendation", verified.recommendation};
      return result_json(parsed, result);
    }
    SolveStatus mapped;
    if (status == "infeasible") mapped = SolveStatus::infeasible;
    else if (status == "invalid_input") mapped = SolveStatus::invalid_input;
    else if (status == "unavailable") mapped = SolveStatus::unavailable;
    else if (status == "solver_failed") mapped = SolveStatus::solver_failed;
    else if (status == "rejected_solution") mapped = SolveStatus::rejected_solution;
    else throw std::runtime_error("worker returned an unsupported status");
    if (reply.contains("recommendation") && !reply["recommendation"].is_null())
      throw std::runtime_error("failed worker result contains a recommendation");
    const auto message = reply.at("error").at("message").get<std::string>();
    if (message.size() > 8192) throw std::runtime_error("worker error detail exceeds limit");
    return result_json(parsed, {mapped, message, std::nullopt});
  }
  void solve(const ParsedRequest& parsed, int timeout_ms) {
    active_id_ = parsed.engine.request_id;
    cancelled_ = false;
    struct ResetActive {
      std::string& id;
      bool& cancelled;
      ~ResetActive() { id.clear(); cancelled = false; }
    } reset_active{active_id_, cancelled_};
    emit({{"event", "accepted"}, {"schema_version", kSchemaVersion}, {"request_id", active_id_},
          {"timeout_ms", timeout_ms}});
    const auto outcome = process::run({worker_, "--ampl-directory", runtime_, "--solver", solver_},
                                      parsed.snapshot.dump(), std::chrono::milliseconds(timeout_ms), [&] {
      pump(); drain(); return cancelled_ || stopping || broken_;
    });
    Json reply;
    if (outcome.status == process::Status::timed_out)
      reply = failure_json(active_id_, "timed_out", "deadline_exceeded", "solve deadline exceeded; worker process group terminated");
    else if (outcome.status == process::Status::cancelled || cancelled_ || stopping)
      reply = failure_json(active_id_, "cancelled", "cancelled", "solve cancelled; worker process group terminated");
    else if (outcome.status != process::Status::completed || outcome.exit_code != 0)
      reply = failure_json(active_id_, "solver_failed", "worker_failed", outcome.error.empty() ? "worker failed without a valid result" : outcome.error);
    else {
      try { reply = checked_reply(parsed, outcome.output); }
      catch (const std::exception& error) {
        reply = failure_json(active_id_, "rejected_solution", "invalid_worker_result", error.what());
      }
    }
    reply["input_snapshot"] = parsed.snapshot;
    reply["policy"] = {{"id", parsed.policy_id}, {"version", parsed.policy_version}};
    reply["execution"] = {{"backend", "supervised_process"}, {"solver", solver_}, {"deadline_ms", timeout_ms},
                          {"elapsed_ms", outcome.elapsed.count()}, {"worker_exit_code", outcome.exit_code},
                          {"diagnostics", outcome.diagnostics}};
    emit(reply);
  }

  std::string worker_, runtime_, solver_, input_, output_, active_id_;
  std::size_t output_offset_ = 0;
  bool eof_ = false, broken_ = false, cancelled_ = false;
};
}  // namespace

int main(int argc, char** argv) {
  try {
    if (argc == 2 && std::string(argv[1]) == "--example") {
      std::cout << Json{{"op", "solve"}, {"timeout_ms", 5000},
                       {"request", synthetic_request_json(unix_now())}}.dump() << '\n';
      return 0;
    }
    std::string worker = (std::filesystem::canonical("/proc/self/exe").parent_path() / "price_ampl_worker").string();
    std::string runtime, solver = "highs";
    for (int i = 1; i < argc; ++i) {
      const std::string option = argv[i];
      if (option == "--help") {
        std::cout << "Usage: price_backend --ampl-directory ABS [--worker ABS] [--solver highs]\n"
                     "JSON Lines on stdin/stdout; solve, cancel and ping commands. --example emits a synthetic solve command.\n";
        return 0;
      }
      if (i + 1 == argc) throw std::invalid_argument("missing backend option value");
      if (option == "--ampl-directory") runtime = argv[++i];
      else if (option == "--worker") worker = argv[++i];
      else if (option == "--solver") solver = argv[++i];
      else throw std::invalid_argument("unknown backend option");
    }
    if (!std::filesystem::path(worker).is_absolute() || !std::filesystem::path(runtime).is_absolute())
      throw std::invalid_argument("worker and AMPL directory must be absolute administrator-configured paths");
    if (solver != "highs") throw std::invalid_argument("only highs is supported");
    struct sigaction action{};
    action.sa_handler = stop;
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, nullptr); sigaction(SIGTERM, &action, nullptr);
    signal(SIGPIPE, SIG_IGN);
    return Service(worker, runtime, solver).serve();
  } catch (const std::exception& error) {
    std::cerr << "price_backend: " << error.what() << '\n';
    return 2;
  }
}
