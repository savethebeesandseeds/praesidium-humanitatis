// SPDX-License-Identifier: MIT
#pragma once

#include <nlohmann/json.hpp>
#include <ph/exponential_smoothing/ewma.hpp>
#include <cstdint>
#include <string>
#include <vector>

namespace ph::exchange::models {
using Amount = std::int64_t;
using Json = nlohmann::json;

struct ConsumerConfig {
  int potential_visitors = 100;
  int visit_probability_bps = 8000;
  Amount budget_min = 500;
  Amount budget_max = 2000;
  int need_probability_bps = 5000;
  int max_units_per_product = 2;
  int choice_scale_bps = 4000;
};
struct ForecastConfig {
  std::string model = "ewma";
  int alpha_bps = 2500;
  int sigma_multiplier_bps = 30000;
  int min_history = 7;
  double prior_sigma_units = 3;
  int reserve_horizon_days = 7;
};
struct ProductPrior {
  std::string sku;
  Amount reference_price = 1;
  int base_demand = 0;
  int elasticity_bps = 10000;
};

ConsumerConfig consumer_config(const Json& object);
ForecastConfig forecast_config(const Json& object);
Json to_json(const ConsumerConfig& config);
Json to_json(const ForecastConfig& config);
void validate(const ConsumerConfig& config);
void validate(const ForecastConfig& config);
void validate(const ProductPrior& product);

// Declared reference demand calibrates the logit intercept before budgets and
// stock constraints. No actual behavior or future simulation draw is fitted.
double purchase_probability(const ConsumerConfig& config, const ProductPrior& product,
                            Amount price);
double price_response_factor(const ConsumerConfig& config, const ProductPrior& product,
                             Amount price);
Json consumer_prior(const ConsumerConfig& config, const ProductPrior& product);

struct ProductConsumption {
  std::string sku;
  int purchase_probability_ppm = 0;
  Amount need_customers = 0;
  Amount no_purchase_customers = 0;
  Amount desired_units = 0;       // Chosen before the budget limit.
  Amount budget_rejected_units = 0;
  Amount requested_units = 0;     // Affordable demand presented to the store.
  Amount sales_units = 0;
  Amount stock_lost_units = 0;
  Amount revenue = 0;
};
struct ConsumerDay {
  int effective_visit_probability_bps = 0;
  Amount potential_visitors = 0;
  Amount visitors = 0;
  Amount no_visit = 0;
  Amount no_purchase = 0;         // Visitors with no fulfilled purchase.
  Amount opening_budget = 0;     // Sum for actual visitors only.
  Amount remaining_budget = 0;
  Amount spending = 0;
  std::vector<ProductConsumption> products;
};

// Stable random keys are (seed, day, customer index, SKU, event type). Product
// order affects budget allocation, but never changes another SKU's random draw.
// demand_factor_bps scales visit probability, capped at one and reported by the
// caller; it does not reveal future shocks to the forecast model.
ConsumerDay consumers_day(const ConsumerConfig& config,
                          const std::vector<ProductPrior>& products,
                          const std::vector<Amount>& prices,
                          const std::vector<Amount>& stock,
                          std::uint32_t seed, int day,
                          int demand_factor_bps = 10000);
Json to_json(const ConsumerDay& day);

using ForecastPoint = ph::exponential_smoothing::ForecastPoint;
Json to_json(const ForecastPoint& forecast);

// Exchange adapter: stock censoring, application config, and the existing JSON
// contract stay here; the reusable exponential smoothing tool owns the model.
class EwmaForecaster {
 public:
  EwmaForecaster(ForecastConfig config, double prior_level);
  // Pure read: forecast depends only on the constructor prior and observations
  // already supplied. Price response is a declared multiplier, not learned here.
  ForecastPoint predict(double price_response = 1, int horizon_days = 1) const;
  // Scores the pre-update one-step forecast, then learns. Day may be negative
  // for explicit pre-run history but must increase strictly. sales==stock is
  // conservatively censored; closed/censored observations never update errors
  // or level. This API accepts sales only, never latent simulated demand.
  // Explicit external uncensored records may use sales+1 as a stock sentinel,
  // so available_stock allows 1,000,001 while sales remain <=1,000,000.
  Json observe(int day, Amount sales, Amount available_stock,
               double price_response = 1, bool trading = true);
  Json diagnostics() const;

 private:
  ph::exponential_smoothing::EwmaForecaster forecaster_;
};
}  // namespace ph::exchange::models
