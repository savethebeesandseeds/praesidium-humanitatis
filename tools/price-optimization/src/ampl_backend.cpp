// SPDX-License-Identifier: LicenseRef-Praesidium-Humanitatis-Worker-Protection-1.0
#include "ph/price/engine.hpp"
#include "ampl_data.hpp"
#include <exception>
#include <utility>

#ifdef PH_PRICE_WITH_AMPL
#include "ampl/ampl.h"
#include "model_text.hpp"
#endif

namespace ph::price {
namespace {
class AmplBackend final : public SolverBackend {
 public:
  explicit AmplBackend(AmplConfig config) : config_(std::move(config)) {}

  RawSolution solve_raw(const Request& request) override {
    if (config_.solver != "highs") {
      return {BackendStatus::failed, {}, 0,
              "unsupported solver: this adapter supports only highs with its reviewed status-code policy"};
    }
    if (config_.binary_directory.empty()) {
      return {BackendStatus::failed, {}, 0, "AMPL needs an explicit runtime directory"};
    }
#ifdef PH_PRICE_WITH_AMPL
    try {
      // Validate before opening the interpreter. Never load caller-supplied
      // model or command text; the reviewed model is embedded at build time.
      const auto data = detail::ampl_data(request);
      ampl::Environment environment(config_.binary_directory.c_str());
      ampl::AMPL ampl(environment);
      ampl.eval(detail::model_text);
      ampl.eval(data.c_str());
      ampl.setOption("solver", config_.solver.c_str());
      try {
        // Exactly one solve in this fresh interpreter: any post-exception
        // status belongs to this request, never to a previous solution.
        ampl.solve();
      } catch (const ampl::InfeasibilityException& error) {
        // Some API versions use this dedicated type. Its PresolveException
        // base also covers other difficulties and is not accepted here.
        return {BackendStatus::infeasible, {}, 0,
                std::string("AMPL presolve detected infeasibility (typed exception): ") + error.what()};
      } catch (const ampl::AMPLException&) {
        // AMPL 20260809 can raise generic AMPLException for presolve failure.
        // AMPL's documented presolve-infeasible code is 299 (changelog,
        // 20210531: https://dev.ampl.com/releases/ampl.html). Confirm both
        // authoritative fields; never classify by exception message text.
        bool presolve_infeasible = false;
        try {
          const auto exception_code = ampl.getValue("solve_result_num").dbl();
          const std::string exception_status = ampl.getValue("solve_result").c_str();
          presolve_infeasible = exception_code == 299 && exception_status == "infeasible";
        } catch (...) {
          // A failed status query cannot establish infeasibility. Rethrow
          // the original solve error below and preserve failure semantics.
        }
        if (presolve_infeasible) {
          return {BackendStatus::infeasible, {}, 0,
                  "AMPL presolve reports infeasible (solve_result_num=299)"};
        }
        throw;
      }
      const auto code = ampl.getValue("solve_result_num").dbl();
      const std::string status = ampl.getValue("solve_result").c_str();
      if (code == 299 && status == "infeasible") {
        return {BackendStatus::infeasible, {}, 0,
                "AMPL presolve reports infeasible (solve_result_num=299)"};
      }
      if (code >= 200 && code < 300 && status == "infeasible") {
        return {BackendStatus::infeasible, {}, 0, "HiGHS reports infeasible under the protected constraints"};
      }
      // HiGHS normal optimal result is 0/solved. Separately, AMPL itself
      // reports 99/solved when presolve completely solves the model (AMPL
      // changelog 20210531). Neither permits skipping independent checks.
      // No other driver status, limit, incumbent, or uncertain result is
      // accepted; do not reuse this mapping for another solver.
      const bool highs_optimal = code == 0 && status == "solved";
      const bool ampl_presolve_solved = code == 99 && status == "solved";
      if (!highs_optimal && !ampl_presolve_solved) {
        return {BackendStatus::failed, {}, 0, "AMPL did not report a normal optimal solution (" + status + ")"};
      }
      RawSolution result;
      result.status = BackendStatus::optimal;
      const auto choices = ampl.getVariable("choose");
      for (std::size_t i = 0; i < request.products.size(); ++i) {
        for (std::size_t k = 0; k < request.products[i].candidates.size(); ++k) {
          result.choices.push_back(choices.get(static_cast<double>(i + 1), static_cast<double>(k + 1)).value());
        }
      }
      result.expected_worker_surplus = ampl.getValue(
          "sum {s in SCENARIOS} probability[s] * scenario_surplus[s]").dbl();
      result.expected_absolute_balance = ampl.getObjective("ExpectedAbsoluteBalance").value();
      result.detail = ampl_presolve_solved
          ? "AMPL presolve solved (solve_result_num=99); awaiting independent verification"
          : "HiGHS optimal; awaiting independent verification";
      return result;
    } catch (const std::exception& error) {
      return {BackendStatus::failed, {}, 0, std::string("AMPL failure: ") + error.what()};
    }
#else
    (void)request;
    return {BackendStatus::unavailable, {}, 0,
            "AMPL backend is disabled; configure PH_PRICE_WITH_AMPL with a separately licensed SDK/runtime/solver"};
#endif
  }
 private:
  AmplConfig config_;
};
}  // namespace

std::unique_ptr<SolverBackend> make_ampl_backend(AmplConfig config) {
  return std::make_unique<AmplBackend>(std::move(config));
}
bool ampl_backend_compiled() {
#ifdef PH_PRICE_WITH_AMPL
  return true;
#else
  return false;
#endif
}
}  // namespace ph::price
