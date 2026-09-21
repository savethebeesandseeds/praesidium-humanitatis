// SPDX-License-Identifier: LicenseRef-Praesidium-Humanitatis-Worker-Protection-1.0
#include "ph/price/integration.hpp"
#include <cerrno>
#include <chrono>
#include <csignal>
#include <fcntl.h>
#include <iostream>
#include <poll.h>
#include <stdexcept>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace {
using namespace ph::price;
using namespace ph::price::integration;
using namespace std::chrono_literals;
int checks = 0;
void require(bool condition, const std::string& message) {
  ++checks;
  if (!condition) throw std::runtime_error(message);
}
class Session {
 public:
  Session(const std::string& service, const std::string& worker, const std::string& runtime) {
    int in[2], out[2];
    if (pipe(in) || pipe(out)) throw std::runtime_error("test pipe failed");
    pid_ = fork();
    if (pid_ < 0) throw std::runtime_error("test fork failed");
    if (pid_ == 0) {
      dup2(in[0], STDIN_FILENO); dup2(out[1], STDOUT_FILENO);
      close(in[0]); close(in[1]); close(out[0]); close(out[1]);
      execl(service.c_str(), service.c_str(), "--worker", worker.c_str(), "--ampl-directory", runtime.c_str(), nullptr);
      _exit(127);
    }
    close(in[0]); close(out[1]); input_ = in[1]; output_ = out[0];
    fcntl(output_, F_SETFL, fcntl(output_, F_GETFL) | O_NONBLOCK);
  }
  ~Session() {
    if (input_ >= 0) close(input_);
    if (output_ >= 0) close(output_);
    if (pid_ > 0) {
      kill(pid_, SIGTERM);
      for (int i = 0; i < 100; ++i) {
        if (waitpid(pid_, nullptr, WNOHANG) == pid_) { pid_ = -1; break; }
        std::this_thread::sleep_for(10ms);
      }
      if (pid_ > 0) { kill(pid_, SIGKILL); waitpid(pid_, nullptr, 0); }
    }
  }
  void send(const Json& value) { send_text(value.dump() + '\n'); }
  void send_text(const std::string& text) {
    std::size_t sent = 0;
    while (sent < text.size()) {
      const auto n = write(input_, text.data() + sent, text.size() - sent);
      if (n < 0 && errno == EINTR) continue;
      if (n <= 0) throw std::runtime_error("backend input closed unexpectedly");
      sent += static_cast<std::size_t>(n);
    }
  }
  Json next() {
    const auto deadline = std::chrono::steady_clock::now() + 8s;
    while (std::chrono::steady_clock::now() < deadline) {
      const auto end = buffered_.find('\n');
      if (end != std::string::npos) {
        const auto line = buffered_.substr(0, end); buffered_.erase(0, end + 1);
        return parse_json(line);
      }
      pollfd fd{output_, POLLIN, 0};
      if (poll(&fd, 1, 50) < 0 && errno != EINTR) throw std::runtime_error("test poll failed");
      char data[8192];
      const auto count = read(output_, data, sizeof data);
      if (count > 0) buffered_.append(data, static_cast<std::size_t>(count));
      else if (count == 0) throw std::runtime_error("backend closed without expected response");
      else if (errno != EAGAIN && errno != EINTR) throw std::runtime_error("test read failed");
      if (buffered_.size() > kMaxJsonBytes) throw std::runtime_error("test response too large");
    }
    throw std::runtime_error("backend response deadline exceeded");
  }
  void eof() { close(input_); input_ = -1; }
  void finish() {
    if (input_ >= 0) eof();
    for (int i = 0; i < 200; ++i) {
      int status = 0;
      if (waitpid(pid_, &status, WNOHANG) == pid_) {
        pid_ = -1;
        require(WIFEXITED(status) && WEXITSTATUS(status) == 0, "backend did not exit cleanly"); return;
      }
      std::this_thread::sleep_for(10ms);
    }
    throw std::runtime_error("backend did not finish on EOF");
  }
  void terminate() { kill(pid_, SIGTERM); }
 private:
  pid_t pid_ = -1;
  int input_ = -1, output_ = -1;
  std::string buffered_;
};
Json solve_command(const std::string& id, int timeout = 5000) {
  auto request = synthetic_request_json(unix_now()); request["request_id"] = id;
  return {{"op", "solve"}, {"timeout_ms", timeout}, {"request", request}};
}
void accepted(Session& session, const std::string& id) {
  const auto value = session.next();
  require(value["event"] == "accepted" && value["request_id"] == id, "missing correlated acceptance: " + value.dump());
}
void failed(const Json& value, const std::string& status, const std::string& id) {
  require(value["event"] == "result" && value["status"] == status && value["request_id"] == id,
          "unexpected failure envelope: " + value.dump());
  require(!value.contains("recommendation") || value["recommendation"].is_null(), "failure exposed a recommendation");
}
void successful(const Json& value, const Json& command) {
  require(value["event"] == "result" && value["status"] == "recommended", "expected recommendation: " + value.dump());
  require(value["input_snapshot"] == command["request"], "snapshot changed across process boundary");
  require(value["request_id"] == command["request"]["request_id"], "result correlation mismatch");
  const auto& rec = value.at("recommendation");
  require(rec["products"][0]["selected_price"] == 200 && rec["products"][1]["selected_price"] == 300,
          "unexpected synthetic prices");
  require(rec["expected"]["worker_surplus"] == 2000, "unexpected expected surplus");
  require(rec["scenarios"][0]["worker_surplus"] == 2400 && rec["scenarios"][1]["worker_surplus"] == 1400,
          "unexpected scenario surplus");
  require(value["model_sha256"].get<std::string>().size() == 64, "missing compiled model digest");
  require(value["execution"]["elapsed_ms"].get<long long>() >= 0, "invalid elapsed time");
}
Json continuity_command(const std::string& id) {
  auto command = solve_command(id);
  command["request"]["policy"]["worker_wage_floor"] = 2500;
  command["request"]["feedback"]["liquidity_buffer"] = 300;
  return command;
}
void cash_continuity(const Json& value, const Json& command) {
  require(value["event"] == "result" && value["status"] == "recommended", "cash-funded continuity failed: " + value.dump());
  require(value["input_snapshot"] == command["request"] && value["schema_version"] == "ph.price.v3",
          "continuity contract or retained snapshot changed");
  const auto& rec = value.at("recommendation");
  require(rec["feedback"]["liquidity_buffer"] == 300 && rec["feedback"]["direction"] == "hold",
          "asset liquidity must preserve neutral price direction");
  require(rec["scenarios"][1]["worker_surplus"] == -300 && rec["scenarios"][1]["funding_balance"] == -300 &&
          rec["scenarios"][1]["coverage"]["slack"] == 0,
          "supervisor did not independently preserve financial deficit and exact liquidity coverage");
}
}  // namespace

int main(int argc, char** argv) {
  signal(SIGPIPE, SIG_IGN);
  try {
    if (argc != 3 && argc != 4 && argc != 5) throw std::runtime_error("Usage: test backend fixture [disabled-worker | real-worker AMPL-directory]");
    const std::string service = argv[1], fixture = argv[2];
    {
      Session session(service, fixture, "/fixture/success");
      session.send({{"op", "ping"}});
      require(session.next()["event"] == "pong", "ping failed");
      session.send_text("{\"op\":\"ping\",\"op\":\"solve\"}\n");
      require(session.next()["error"]["code"] == "invalid_command", "duplicate command key accepted");
      session.send({{"op", "cancel"}, {"request_id", "unknown"}});
      require(session.next()["error"]["code"] == "not_found", "missing cancellation target not distinguished");
      auto expired = solve_command("expired"); expired["request"]["as_of"] = unix_now() - 1000;
      expired["request"]["valid_until"] = unix_now() - 1;
      session.send(expired); failed(session.next(), "invalid_input", "expired");
      auto unknown = solve_command("unknown-field"); unknown["request"]["arbitrary_ampl_code"] = "solve;";
      session.send(unknown); failed(session.next(), "invalid_input", "unknown-field");
      const auto command = solve_command("fixture-success"); session.send(command);
      accepted(session, "fixture-success"); successful(session.next(), command);
      const auto continuity = continuity_command("fixture-continuity"); session.send(continuity);
      accepted(session, "fixture-continuity"); cash_continuity(session.next(), continuity); session.finish();
    }
    {
      Session session(service, fixture, "/fixture/hang");
      session.send(solve_command("cancel-me")); accepted(session, "cancel-me");
      session.send_text(std::string(5000, ' ') + "{\"op\":\"ping\"}\n");
      require(session.next()["error"]["code"] == "active_command_too_large", "large active command was not bounded");
      session.send(solve_command("second"));
      require(session.next()["error"]["code"] == "busy", "active worker was silently replaced");
      session.send({{"op", "cancel"}, {"request_id", "cancel-me"}});
      require(session.next()["event"] == "cancellation_requested", "cancel acknowledgement absent");
      failed(session.next(), "cancelled", "cancel-me");
      session.send(solve_command("timeout", 40)); accepted(session, "timeout");
      const auto timed = session.next(); failed(timed, "timed_out", "timeout");
      require(timed["execution"]["elapsed_ms"].get<long long>() < 1500, "deadline did not stop blocked worker promptly");
      session.finish();
    }
    for (const auto& mode : {"malformed", "forged", "forged-balance", "wrong-id", "wrong-snapshot", "wrong-model", "wrong-objective"}) {
      Session session(service, fixture, "/fixture/" + std::string(mode));
      session.send(solve_command(mode)); accepted(session, mode);
      failed(session.next(), "rejected_solution", mode); session.finish();
    }
    {
      Session session(service, fixture, "/fixture/invalid-diagnostic");
      const auto command = solve_command("invalid-diagnostic"); session.send(command);
      accepted(session, "invalid-diagnostic");
      const auto result = session.next(); successful(result, command);
      require(result["execution"]["diagnostics"].get<std::string>().find("\xEF\xBF\xBD") != std::string::npos,
              "invalid diagnostic byte was not replaced in valid JSON");
      session.finish();
    }
    {
      Session session(service, fixture, "/fixture/failure");
      session.send(solve_command("worker-failure")); accepted(session, "worker-failure");
      failed(session.next(), "solver_failed", "worker-failure"); session.finish();
    }
    {
      Session session(service, fixture, "/fixture/success");
      auto command = solve_command("eof-success"); session.send(command); session.eof();
      accepted(session, "eof-success"); successful(session.next(), command); session.finish();
    }
    {
      Session session(service, fixture, "/fixture/hang");
      session.send(solve_command("terminate")); accepted(session, "terminate");
      session.terminate(); session.finish();
    }
    if (argc == 4) {
      Session unavailable(service, argv[3], "/fixture/absent-runtime");
      unavailable.send(solve_command("ampl-disabled")); accepted(unavailable, "ampl-disabled");
      failed(unavailable.next(), "unavailable", "ampl-disabled"); unavailable.finish();
    }
    if (argc == 5) {
      Session real(service, argv[3], argv[4]);
      const auto command = solve_command("real-ampl"); real.send(command);
      accepted(real, "real-ampl"); successful(real.next(), command);
      auto continuity = continuity_command("real-continuity"); real.send(continuity);
      accepted(real, "real-continuity"); cash_continuity(real.next(), continuity);
      continuity["request"]["request_id"] = "real-insufficient-liquidity";
      continuity["request"]["feedback"]["liquidity_buffer"] = 299;
      real.send(continuity); accepted(real, "real-insufficient-liquidity");
      failed(real.next(), "infeasible", "real-insufficient-liquidity");
      auto infeasible = solve_command("real-infeasible");
      infeasible["request"]["policy"]["worker_wage_floor"] = 100000;
      real.send(infeasible); accepted(real, "real-infeasible"); failed(real.next(), "infeasible", "real-infeasible");
      real.send(solve_command("real-timeout", 1)); accepted(real, "real-timeout");
      failed(real.next(), "timed_out", "real-timeout");
      real.send(solve_command("real-cancel")); accepted(real, "real-cancel");
      real.send({{"op", "cancel"}, {"request_id", "real-cancel"}});
      require(real.next()["event"] == "cancellation_requested", "real cancel acknowledgement absent");
      failed(real.next(), "cancelled", "real-cancel");
      // Confirm the service remains usable after terminating prior solve trees.
      const auto again = solve_command("real-recovery"); real.send(again);
      accepted(real, "real-recovery"); successful(real.next(), again); real.finish();
    }
    std::cout << checks << " backend boundary checks passed" << (argc == 5 ? " including real AMPL success/infeasibility/timeout/cancellation/recovery\n" :
        argc == 4 ? " including the AMPL-disabled worker with no fallback\n" : " using explicit protocol fixtures\n");
    return 0;
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
