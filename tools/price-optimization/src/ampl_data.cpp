// SPDX-License-Identifier: LicenseRef-Praesidium-Humanitatis-Worker-Protection-1.0
#include "ampl_data.hpp"
#include <iomanip>
#include <locale>
#include <sstream>
#include <stdexcept>

namespace ph::price::detail {
std::string ampl_data(const Request& r) {
  if (!validate_request(r, r.as_of).ok()) throw std::invalid_argument("cannot encode invalid AMPL input");
  std::ostringstream out;
  out.imbue(std::locale::classic());
  out << std::setprecision(17);
  out << "let product_count := " << r.products.size() << ";\n";
  out << "let scenario_count := " << r.scenarios.size() << ";\n";
  out << "let worker_wage_floor := " << r.worker_wage_floor << ";\n";
  out << "let operating_cost := " << r.operating_cost << ";\n";
  out << "let reserve_floor := " << r.reserve_floor << ";\n";
  out << "let funding_balance := " << r.funding_balance << ";\n";
  out << "let coverage_credit := " << r.coverage_credit << ";\n";
  out << "let liquidity_buffer := " << r.liquidity_buffer << ";\n";
  for (std::size_t s = 0; s < r.scenarios.size(); ++s) {
    out << "let probability[" << s + 1 << "] := " << r.scenarios[s].probability << ";\n";
  }
  // CHOICES depends on every candidate_count. Initialize the complete array
  // before assigning any parameter indexed by CHOICES.
  for (std::size_t i = 0; i < r.products.size(); ++i) {
    out << "let candidate_count[" << i + 1 << "] := " << r.products[i].candidates.size() << ";\n";
  }
  for (std::size_t i = 0; i < r.products.size(); ++i) {
    const auto& p = r.products[i];
    const auto n = i + 1;
    out << "let unit_cost[" << n << "] := " << p.unit_cost << ";\n";
    out << "let previous_price[" << n << "] := " << p.previous_price << ";\n";
    out << "let affordability_ceiling[" << n << "] := " << p.affordability_ceiling << ";\n";
    out << "let inventory[" << n << "] := " << p.inventory << ";\n";
    out << "let max_change_bp[" << n << "] := " << p.max_change_basis_points << ";\n";
    for (std::size_t k = 0; k < p.candidates.size(); ++k) {
      const auto& c = p.candidates[k];
      if (c.price == p.previous_price) out << "let hold_candidate[" << n << "] := " << k + 1 << ";\n";
      out << "let price[" << n << ',' << k + 1 << "] := " << c.price << ";\n";
      for (std::size_t s = 0; s < r.scenarios.size(); ++s) {
        out << "let forecast_units[" << n << ',' << k + 1 << ',' << s + 1 << "] := "
            << c.forecast_units[s] << ";\n";
      }
    }
  }
  return out.str();
}
}
